#include "voice_effects.h"
#include <onnxruntime_cxx_api.h>
#ifdef AUDIOBRIDGE_HAS_DIRECTML
#include <Windows.h>
#include <dml_provider_factory.h>
#include <dxgi.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
constexpr size_t DefaultHop = 512, Rate = 48000, MaxClip = 10 * Rate;
constexpr size_t DfnHop = 512, DfnDelay = 1536;
constexpr size_t FastEnhancerHop = 200, FastEnhancerDelay = 824;
constexpr size_t DpdfWin = 960, DpdfHop = 480, DpdfBins = DpdfWin / 2 + 1, DpdfDelay = 2400;
constexpr size_t InworldWin = 2048, InworldHop = 480, InworldStepFrames = 2, InworldBatchHop = InworldHop * InworldStepFrames;
constexpr size_t InworldBins = InworldWin / 2 + 1;
constexpr size_t InworldLookahead = 2, InworldAttnWindow = 256;
constexpr size_t InworldDelay = (InworldWin - InworldHop) + InworldLookahead * InworldHop; // 2528 samples (52.7 ms)
constexpr uint16_t Fp16Zero = 0x0000;
constexpr uint16_t Fp16Neg10000 = 0xF0E2; // -10000.0 in IEEE-754 binary16
constexpr double Pi = 3.14159265358979323846;

size_t modelHop(int model) {
    switch (model) {
    case VoiceSettings::ModelFastEnhancerL: return FastEnhancerHop;
    case VoiceSettings::ModelDpdfNet8: return DpdfHop;
    case VoiceSettings::ModelInworldGpu: return InworldBatchHop;
    default: return DfnHop;
    }
}
size_t modelDelay(int model) {
    switch (model) {
    case VoiceSettings::ModelFastEnhancerL: return FastEnhancerDelay;
    case VoiceSettings::ModelDpdfNet8: return DpdfDelay;
    case VoiceSettings::ModelInworldGpu: return InworldDelay;
    default: return DfnDelay;
    }
}

Ort::Env& environment() { static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "AudioBridge voice"); return env; }
Ort::SessionOptions sessionOptions(int intraThreads = 1) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(intraThreads);
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return options;
}

#ifdef AUDIOBRIDGE_HAS_DIRECTML
std::string wideToUtf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::vector<VoiceGpuDevice> listDmlDevices() {
    std::vector<VoiceGpuDevice> devices;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return devices;
    for (UINT idx = 0;; ++idx) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            VoiceGpuDevice dev;
            dev.id = static_cast<int>(idx);
            dev.name = wideToUtf8(desc.Description);
            dev.vramMb = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ULL * 1024ULL);
            devices.push_back(std::move(dev));
        }
        adapter->Release();
    }
    factory->Release();
    return devices;
}

int resolveDmlDeviceId(int requested) {
    const auto devices = listDmlDevices();
    if (devices.empty()) return std::max(0, requested);
    if (requested >= 0) {
        for (const auto& d : devices) {
            if (d.id == requested) return d.id;
        }
    }
    int bestIndex = devices.front().id;
    uint64_t bestVram = devices.front().vramMb;
    for (const auto& d : devices) {
        if (d.vramMb > bestVram) {
            bestVram = d.vramMb;
            bestIndex = d.id;
        }
    }
    return bestIndex;
}

std::string gpuDeviceName(int deviceId) {
    for (const auto& d : listDmlDevices()) {
        if (d.id == deviceId) return d.name;
    }
    return "GPU " + std::to_string(deviceId);
}

Ort::SessionOptions dmlSessionOptions(int deviceId) {
    Ort::SessionOptions options;
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.DisableMemPattern();
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    const OrtApi& api = Ort::GetApi();
    const OrtDmlApi* dmlApi = nullptr;
    Ort::ThrowOnError(api.GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));
    Ort::ThrowOnError(dmlApi->SessionOptionsAppendExecutionProvider_DML(options, deviceId));
    return options;
}
#endif

// Self-contained 960-point (64 x 15) real FFT/IRFFT and Vorbis window for DPDFNet8 HR.
struct Rfft960 {
    std::array<float, DpdfWin> vorbis{};
    std::array<std::complex<double>, 32> w64{};
    std::array<std::array<std::complex<double>, 64>, 15> w960{};
    std::array<std::array<std::complex<double>, 15>, 15> w15{};
    std::array<size_t, 64> bitrev{};

    Rfft960() {
        for (size_t i = 0; i < DpdfWin; ++i) {
            const double s = std::sin(0.5 * Pi * (static_cast<double>(i) + 0.5) / static_cast<double>(DpdfHop));
            vorbis[i] = static_cast<float>(std::sin(0.5 * Pi * s * s));
        }
        for (size_t i = 0; i < 32; ++i) {
            const double a = -2.0 * Pi * static_cast<double>(i) / 64.0;
            w64[i] = {std::cos(a), std::sin(a)};
        }
        for (size_t k = 0; k < 15; ++k) {
            for (size_t p = 0; p < 64; ++p) {
                const double a = -2.0 * Pi * static_cast<double>(k * p) / 960.0;
                w960[k][p] = {std::cos(a), std::sin(a)};
            }
            for (size_t q = 0; q < 15; ++q) {
                const double a = -2.0 * Pi * static_cast<double>(k * q) / 15.0;
                w15[q][k] = {std::cos(a), std::sin(a)};
            }
        }
        for (size_t i = 0; i < 64; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < 6; ++b) r = (r << 1) | ((i >> b) & 1);
            bitrev[i] = r;
        }
    }

    void fft64(std::complex<double>* a) const {
        for (size_t i = 0; i < 64; ++i) {
            if (i < bitrev[i]) std::swap(a[i], a[bitrev[i]]);
        }
        for (size_t len = 2; len <= 64; len <<= 1) {
            const size_t half = len >> 1, step = 64 / len;
            for (size_t i = 0; i < 64; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    const auto u = a[i + j];
                    const auto v = a[i + j + half] * w64[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

    void fft960(const std::complex<double>* in, std::complex<double>* out) const {
        std::array<std::array<std::complex<double>, 64>, 15> g{};
        for (size_t k = 0; k < 15; ++k) {
            auto& row = g[k];
            for (size_t m = 0; m < 64; ++m) row[m] = in[15 * m + k];
            fft64(row.data());
            for (size_t p = 0; p < 64; ++p) row[p] *= w960[k][p];
        }
        for (size_t q = 0; q < 15; ++q) {
            const auto& wq = w15[q];
            for (size_t p = 0; p < 64; ++p) {
                std::complex<double> sum{};
                for (size_t k = 0; k < 15; ++k) sum += g[k][p] * wq[k];
                out[64 * q + p] = sum;
            }
        }
    }

    void rfft(const float* time960, float* spec481x2) const {
        std::array<std::complex<double>, DpdfWin> in{}, out{};
        for (size_t i = 0; i < DpdfWin; ++i) in[i] = {static_cast<double>(time960[i]), 0.0};
        fft960(in.data(), out.data());
        for (size_t f = 0; f < DpdfBins; ++f) {
            spec481x2[2 * f] = static_cast<float>(out[f].real());
            spec481x2[2 * f + 1] = static_cast<float>(out[f].imag());
        }
    }

    void irfft(const float* spec481x2, float* time960) const {
        std::array<std::complex<double>, DpdfWin> in{}, out{};
        in[0] = {static_cast<double>(spec481x2[0]), 0.0};
        in[DpdfHop] = {static_cast<double>(spec481x2[2 * DpdfHop]), 0.0};
        for (size_t f = 1; f < DpdfHop; ++f) {
            const double re = spec481x2[2 * f], im = spec481x2[2 * f + 1];
            in[f] = {re, -im};
            in[DpdfWin - f] = {re, im};
        }
        fft960(in.data(), out.data());
        constexpr double inv = 1.0 / static_cast<double>(DpdfWin);
        for (size_t n = 0; n < DpdfWin; ++n) time960[n] = static_cast<float>(out[n].real() * inv);
    }
};
const Rfft960& rfft960() { static const Rfft960 instance; return instance; }

// Self-contained 2048-point radix-2 real FFT/IRFFT and periodic Hann window for Inworld Realtime.
struct Rfft2048 {
    std::array<float, InworldWin> hann{};
    std::array<float, InworldWin> hannSq{};
    std::array<std::complex<double>, InworldWin / 2> twiddle{};
    std::array<size_t, InworldWin> bitrev{};

    Rfft2048() {
        for (size_t i = 0; i < InworldWin; ++i) {
            const double w = 0.5 * (1.0 - std::cos(2.0 * Pi * static_cast<double>(i) / static_cast<double>(InworldWin)));
            hann[i] = static_cast<float>(w);
            hannSq[i] = static_cast<float>(w * w);
            size_t r = 0;
            for (size_t b = 0; b < 11; ++b) r = (r << 1) | ((i >> b) & 1);
            bitrev[i] = r;
        }
        for (size_t i = 0; i < InworldWin / 2; ++i) {
            const double a = -2.0 * Pi * static_cast<double>(i) / static_cast<double>(InworldWin);
            twiddle[i] = {std::cos(a), std::sin(a)};
        }
    }

    void fft2048(std::complex<double>* a) const {
        for (size_t i = 0; i < InworldWin; ++i) {
            if (i < bitrev[i]) std::swap(a[i], a[bitrev[i]]);
        }
        for (size_t len = 2; len <= InworldWin; len <<= 1) {
            const size_t half = len >> 1, step = InworldWin / len;
            for (size_t i = 0; i < InworldWin; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    const auto u = a[i + j];
                    const auto v = a[i + j + half] * twiddle[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

    void rfft(const float* time2048, float* spec1025x2) const {
        std::array<std::complex<double>, InworldWin> buf{};
        for (size_t i = 0; i < InworldWin; ++i) buf[i] = {static_cast<double>(time2048[i]), 0.0};
        fft2048(buf.data());
        for (size_t f = 0; f < InworldBins; ++f) {
            spec1025x2[2 * f] = static_cast<float>(buf[f].real());
            spec1025x2[2 * f + 1] = static_cast<float>(buf[f].imag());
        }
    }

    void irfft(const float* spec1025x2, float* time2048) const {
        std::array<std::complex<double>, InworldWin> buf{};
        buf[0] = {static_cast<double>(spec1025x2[0]), 0.0};
        buf[InworldWin / 2] = {static_cast<double>(spec1025x2[2 * (InworldWin / 2)]), 0.0};
        for (size_t f = 1; f < InworldWin / 2; ++f) {
            const double re = spec1025x2[2 * f], im = spec1025x2[2 * f + 1];
            buf[f] = {re, -im};
            buf[InworldWin - f] = {re, im};
        }
        fft2048(buf.data());
        constexpr double inv = 1.0 / static_cast<double>(InworldWin);
        for (size_t n = 0; n < InworldWin; ++n) time2048[n] = static_cast<float>(buf[n].real() * inv);
    }
};
const Rfft2048& rfft2048() { static const Rfft2048 instance; return instance; }

std::vector<float> parseCsvFloats(const std::string& text) {
    std::vector<float> values;
    std::istringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (!token.empty()) values.push_back(std::stof(token));
    }
    return values;
}

std::string quote(const std::string& text) {
    std::string s = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') s += '\\';
        if (c >= 32) s += static_cast<char>(c);
    }
    return s + '"';
}
std::string base64(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned n = static_cast<unsigned>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= static_cast<unsigned>(bytes[i+1]) << 8;
        if (i + 2 < bytes.size()) n |= bytes[i+2];
        out += alphabet[(n >> 18) & 63]; out += alphabet[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
    }
    return out;
}
std::string wav(const std::vector<float>& samples) {
    std::vector<unsigned char> bytes;
    auto word = [&](unsigned n, int count) { for (int i=0;i<count;++i) bytes.push_back((n >> (8*i)) & 255); };
    auto tag = [&](const char* t) { bytes.insert(bytes.end(), t, t+4); };
    tag("RIFF"); word(36 + static_cast<unsigned>(samples.size()*2),4); tag("WAVE");
    tag("fmt "); word(16,4); word(1,2); word(1,2); word(Rate,4); word(Rate*2,4); word(2,2); word(16,2);
    tag("data"); word(static_cast<unsigned>(samples.size()*2),4);
    for (float v : samples) word(static_cast<unsigned>(static_cast<int16_t>(
        std::lround(std::clamp(std::isfinite(v) ? v : 0.f,-1.f,1.f)*32767))),2);
    return "data:audio/wav;base64," + base64(bytes);
}
}

VoiceSettings VoiceSettings::parse(const std::string& text) {
    VoiceSettings s;
    std::istringstream in(text);
    int n, strength, g, threshold, hold, low;
    if (!(in >> n >> strength >> g >> threshold >> hold >> low)) return s;
    s.noise = n != 0; s.strength = std::clamp(strength,0,100);
    s.gate = g != 0; s.threshold = std::clamp(threshold,10,90);
    s.holdMs = std::clamp(hold,100,1500); s.lowCut = low != 0;
    int makeupDb, model, gpuDevice;
    if (in >> makeupDb) {
        s.makeupDb = std::clamp(makeupDb, 0, 12);
        if (in >> model) {
            s.model = std::clamp(model, 0, ModelCount - 1);
            if (in >> gpuDevice) s.gpuDevice = std::clamp(gpuDevice, -1, 31);
        }
    }
    return s;
}
std::string VoiceSettings::serialize() const {
    const int clampedModel = std::clamp(model, 0, ModelCount - 1);
    const int clampedGpu = std::clamp(gpuDevice, -1, 31);
    std::string out = std::to_string(noise) + " " + std::to_string(strength) + " " +
        std::to_string(gate) + " " + std::to_string(threshold) + " " +
        std::to_string(holdMs) + " " + std::to_string(lowCut);
    if (clampedGpu >= 0) {
        out += " " + std::to_string(makeupDb) + " " + std::to_string(clampedModel) + " " + std::to_string(clampedGpu);
    } else if (clampedModel != ModelDeepFilterNet3) {
        out += " " + std::to_string(makeupDb) + " " + std::to_string(clampedModel);
    } else if (makeupDb) {
        out += " " + std::to_string(makeupDb);
    }
    return out;
}

static float softLimit(float x) {
    constexpr float knee = 0.89f, range = 1.f - knee;
    const float a = std::abs(x);
    if (a <= knee) return x;
    return std::copysign(knee + range * std::tanh((a - knee) / range), x);
}

struct VoiceDsp::Impl {
    std::wstring modelDir;
    Ort::Session vad;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // DeepFilterNet3 (model 0)
    std::unique_ptr<Ort::Session> dfn;
    std::vector<std::string> dfnInputNames, dfnOutputNames;
    std::vector<const char*> dfnInputs, dfnOutputs;
    std::vector<std::vector<int64_t>> dfnShapes;
    std::vector<std::vector<float>> dfnStates;

    // FastEnhancer-L 48k (model 1)
    std::unique_ptr<Ort::Session> fe;
    std::vector<std::string> feInputNames, feOutputNames;
    std::vector<const char*> feInputs, feOutputs;
    std::vector<std::vector<int64_t>> feShapes;
    std::vector<std::vector<float>> feStates;

    // DPDFNet8 HR 48k (model 2)
    std::unique_ptr<Ort::Session> dpdf;
    std::string dpdfSpecInName, dpdfStateInName, dpdfSpecOutName, dpdfStateOutName;
    std::vector<float> dpdfInitState, dpdfState;
    std::array<float, DpdfWin> dpdfAnalysis{}, dpdfOla{};

    // Inworld Voice Isolator Realtime FP16 on DirectML GPU (model 3)
    std::unique_ptr<Ort::Session> inworld;
    std::unique_ptr<Ort::MemoryInfo> inworldDmlMem;
    std::unique_ptr<Ort::Allocator> inworldDmlAlloc;
    std::vector<std::string> inworldInputNames, inworldOutputNames;
    std::vector<std::vector<int64_t>> inworldStateShapes;
    std::vector<ONNXTensorElementDataType> inworldStateTypes;
    std::vector<std::vector<Ort::Float16_t>> inworldZeroFp16;
    std::vector<std::vector<float>> inworldZeroFp32;
    std::vector<Ort::Value> inworldZeroTensors;
    std::array<float, InworldStepFrames * InworldBins * 2> inworldSpecBuf{};
    std::array<Ort::Float16_t, InworldStepFrames * InworldAttnWindow> inworldMaskBuf{};
    std::array<float, InworldStepFrames * InworldBins * 4> inworldRawBuf{};
    Ort::Value inworldSpecTensor{nullptr}, inworldMaskTensor{nullptr}, inworldRawTensor{nullptr};
    // Persistent pre-allocated GPU-resident ping-pong state buffers & IoBindings (zero alloc/copy per hop!).
    std::array<std::vector<Ort::Value>, 2> inworldGpuStates;
    std::unique_ptr<Ort::IoBinding> inworldInitBinding;
    std::array<std::unique_ptr<Ort::IoBinding>, 2> inworldBindings;
    int inworldDeviceId = -1;
    int inworldPing = 0;
    std::array<float, InworldWin> inworldAnalysis{}, inworldOla{}, inworldEnv{};
    std::deque<std::array<float, InworldBins * 2>> inworldSpecQueue;
    std::vector<float> inworldDry;
    size_t inworldFrameCount = 0;

    std::vector<float> pending, dry, vadPending;
    std::array<float,256> vadState{};
    std::array<float,64> context{};
    float gain = 1.f;
    float limiterGain = 1.f;
    size_t sinceSpeech = 0;
    bool speech = true;
    double z1 = 0, z2 = 0;
    VoiceSettings previous;
    bool haveSettings = false;
    size_t currentDelay = DfnDelay;

    explicit Impl(const std::wstring& dir, int initialModel, int initialGpu)
        : modelDir(dir),
          vad(environment(), (std::filesystem::path(dir)/L"silero_vad.onnx").c_str(), sessionOptions(1)) {
        if (initialModel >= 0) {
            const int clamped = std::clamp(initialModel, 0, VoiceSettings::ModelCount - 1);
            ensureModel(clamped, initialGpu);
            reset(modelDelay(clamped));
        } else {
            reset(0);
        }
    }

    void releaseInworld() {
        inworldInitBinding.reset();
        inworldBindings[0].reset();
        inworldBindings[1].reset();
        inworldGpuStates[0].clear();
        inworldGpuStates[1].clear();
        inworldZeroTensors.clear();
        inworldZeroFp16.clear();
        inworldZeroFp32.clear();
        inworldSpecTensor = Ort::Value{nullptr};
        inworldMaskTensor = Ort::Value{nullptr};
        inworldRawTensor = Ort::Value{nullptr};
        inworldDmlAlloc.reset();
        inworldDmlMem.reset();
        inworld.reset();
        inworldDeviceId = -1;
    }

    void ensureModel(int model, int gpuDevice = -1) {
        switch (model) {
        case VoiceSettings::ModelFastEnhancerL:
            if (!fe) loadFastEnhancer();
            break;
        case VoiceSettings::ModelDpdfNet8:
            if (!dpdf) loadDpdfNet();
            break;
        case VoiceSettings::ModelInworldGpu: {
#ifdef AUDIOBRIDGE_HAS_DIRECTML
            const int targetDev = resolveDmlDeviceId(gpuDevice);
            if (!inworld || inworldDeviceId != targetDev) loadInworld(targetDev);
#else
            (void)gpuDevice;
            if (!inworld) loadInworld(0);
#endif
            break;
        }
        default:
            if (!dfn) loadDfn();
            break;
        }
    }

    void loadDfn() {
        auto session = std::make_unique<Ort::Session>(
            environment(), (std::filesystem::path(modelDir)/L"deepfilternet3_streaming.onnx").c_str(), sessionOptions(1));
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<std::string> inNames, outNames;
        std::vector<std::vector<int64_t>> shapes;
        for (size_t i = 0; i < session->GetInputCount(); ++i) {
            inNames.emplace_back(session->GetInputNameAllocated(i, allocator).get());
            shapes.push_back(session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        }
        for (size_t i = 0; i < session->GetOutputCount(); ++i)
            outNames.emplace_back(session->GetOutputNameAllocated(i, allocator).get());
        if (inNames.empty() || inNames[0] != "input_frame" || outNames.size() != inNames.size())
            throw std::runtime_error("Unsupported DeepFilterNet model signature");
        for (size_t i = 1; i < shapes.size(); ++i)
            for (auto d : shapes[i])
                if (d <= 0) throw std::runtime_error("Dynamic DeepFilterNet state shape");
        dfn = std::move(session);
        dfnInputNames = std::move(inNames);
        dfnOutputNames = std::move(outNames);
        dfnShapes = std::move(shapes);
        dfnInputs.clear(); dfnOutputs.clear();
        for (const auto& n : dfnInputNames) dfnInputs.push_back(n.c_str());
        for (const auto& n : dfnOutputNames) dfnOutputs.push_back(n.c_str());
        resetDfnState();
    }

    void loadFastEnhancer() {
        auto session = std::make_unique<Ort::Session>(
            environment(), (std::filesystem::path(modelDir)/L"fastenhancer_l.onnx").c_str(), sessionOptions(2));
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<std::string> inNames, outNames;
        std::vector<std::vector<int64_t>> shapes;
        for (size_t i = 0; i < session->GetInputCount(); ++i) {
            inNames.emplace_back(session->GetInputNameAllocated(i, allocator).get());
            shapes.push_back(session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        }
        for (size_t i = 0; i < session->GetOutputCount(); ++i)
            outNames.emplace_back(session->GetOutputNameAllocated(i, allocator).get());
        if (inNames.size() < 2 || outNames.size() != inNames.size() ||
            shapes[0].size() != 2 || shapes[0][0] != 1 || shapes[0][1] != static_cast<int64_t>(FastEnhancerHop))
            throw std::runtime_error("Unsupported FastEnhancer-L model signature");
        for (size_t i = 1; i < shapes.size(); ++i)
            for (auto d : shapes[i])
                if (d <= 0) throw std::runtime_error("Dynamic FastEnhancer-L state shape");
        fe = std::move(session);
        feInputNames = std::move(inNames);
        feOutputNames = std::move(outNames);
        feShapes = std::move(shapes);
        feInputs.clear(); feOutputs.clear();
        for (const auto& n : feInputNames) feInputs.push_back(n.c_str());
        for (const auto& n : feOutputNames) feOutputs.push_back(n.c_str());
        resetFastEnhancerState();
    }

    void loadDpdfNet() {
        auto session = std::make_unique<Ort::Session>(
            environment(), (std::filesystem::path(modelDir)/L"dpdfnet8_48khz_hr.onnx").c_str(), sessionOptions(2));
        Ort::AllocatorWithDefaultOptions allocator;
        if (session->GetInputCount() < 2 || session->GetOutputCount() < 2)
            throw std::runtime_error("Unsupported DPDFNet8 HR model signature");
        auto specShape = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        auto stateShape = session->GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
        if (specShape.size() != 4 || specShape[0] != 1 || specShape[1] != 1 ||
            specShape[2] != static_cast<int64_t>(DpdfBins) || specShape[3] != 2 ||
            stateShape.size() != 1 || stateShape[0] <= 0)
            throw std::runtime_error("Unsupported DPDFNet8 HR tensor shape");

        auto meta = session->GetModelMetadata();
        auto metaString = [&](const char* key) -> std::string {
            auto value = meta.LookupCustomMetadataMapAllocated(key, allocator);
            if (!value) throw std::runtime_error(std::string("DPDFNet8 HR metadata missing: ") + key);
            return value.get();
        };
        const size_t stateSize = static_cast<size_t>(std::stoull(metaString("state_size")));
        const size_t erbSize = static_cast<size_t>(std::stoull(metaString("erb_norm_state_size")));
        const size_t specSize = static_cast<size_t>(std::stoull(metaString("spec_norm_state_size")));
        const auto erbInit = parseCsvFloats(metaString("erb_norm_init"));
        const auto specInit = parseCsvFloats(metaString("spec_norm_init"));
        if (stateSize != static_cast<size_t>(stateShape[0]) ||
            erbInit.size() != erbSize || specInit.size() != specSize ||
            erbSize + specSize > stateSize)
            throw std::runtime_error("Invalid DPDFNet8 HR initial state metadata");

        std::vector<float> initState(stateSize, 0.f);
        std::copy(erbInit.begin(), erbInit.end(), initState.begin());
        std::copy(specInit.begin(), specInit.end(), initState.begin() + erbSize);

        dpdfSpecInName = session->GetInputNameAllocated(0, allocator).get();
        dpdfStateInName = session->GetInputNameAllocated(1, allocator).get();
        dpdfSpecOutName = session->GetOutputNameAllocated(0, allocator).get();
        dpdfStateOutName = session->GetOutputNameAllocated(1, allocator).get();
        dpdfInitState = std::move(initState);
        dpdf = std::move(session);
        resetDpdfState();
    }

    void loadInworld(int deviceId) {
#ifndef AUDIOBRIDGE_HAS_DIRECTML
        (void)deviceId;
        throw std::runtime_error("Inworld Realtime (GPU) requires a DirectML build");
#else
        releaseInworld();
        const auto modelPath = std::filesystem::path(modelDir) / L"inworld_realtime_fp16.onnx";
        if (!std::filesystem::exists(modelPath))
            throw std::runtime_error("Missing voice-models/inworld_realtime_fp16.onnx");
        auto session = std::make_unique<Ort::Session>(environment(), modelPath.c_str(), dmlSessionOptions(deviceId));
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<std::string> inNames, outNames;
        std::vector<std::vector<int64_t>> stateShapes;
        std::vector<ONNXTensorElementDataType> stateTypes;
        for (size_t i = 0; i < session->GetInputCount(); ++i)
            inNames.emplace_back(session->GetInputNameAllocated(i, allocator).get());
        for (size_t i = 0; i < session->GetOutputCount(); ++i)
            outNames.emplace_back(session->GetOutputNameAllocated(i, allocator).get());
        if (inNames.size() != 16 || outNames.size() != 15)
            throw std::runtime_error("Unsupported Inworld Realtime model signature");
        std::unique_ptr<Ort::MemoryInfo> dmlMem;
        std::unique_ptr<Ort::Allocator> dmlAlloc;
        try {
            dmlMem = std::make_unique<Ort::MemoryInfo>("DML", OrtDeviceAllocator, deviceId, OrtMemTypeDefault);
            dmlAlloc = std::make_unique<Ort::Allocator>(*session, *dmlMem);
        } catch (const Ort::Exception&) {
            // DirectML EP registers its session allocator under memory-info id 0 even for secondary GPUs.
            dmlMem = std::make_unique<Ort::MemoryInfo>("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
            dmlAlloc = std::make_unique<Ort::Allocator>(*session, *dmlMem);
        }

        std::vector<std::vector<Ort::Float16_t>> zero16(14);
        std::vector<std::vector<float>> zero32(14);
        std::vector<Ort::Value> zeroTensors;
        zeroTensors.reserve(14);
        std::array<std::vector<Ort::Value>, 2> gpuStates;
        gpuStates[0].reserve(14);
        gpuStates[1].reserve(14);
        for (size_t i = 2; i < inNames.size(); ++i) {
            auto info = session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            auto type = info.GetElementType();
            size_t count = 1;
            for (auto d : shape) {
                if (d <= 0) throw std::runtime_error("Dynamic Inworld state shape");
                count *= static_cast<size_t>(d);
            }
            if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
                zero16[i - 2].assign(count, Ort::Float16_t::FromBits(Fp16Zero));
                zeroTensors.push_back(Ort::Value::CreateTensor<Ort::Float16_t>(
                    memory, zero16[i - 2].data(), zero16[i - 2].size(), shape.data(), shape.size()));
            } else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                zero32[i - 2].assign(count, 0.f);
                zeroTensors.push_back(Ort::Value::CreateTensor<float>(
                    memory, zero32[i - 2].data(), zero32[i - 2].size(), shape.data(), shape.size()));
            } else {
                throw std::runtime_error("Unsupported Inworld state tensor dtype");
            }
            for (int p = 0; p < 2; ++p)
                gpuStates[p].push_back(Ort::Value::CreateTensor(*dmlAlloc, shape.data(), shape.size(), type));
            stateShapes.push_back(std::move(shape));
            stateTypes.push_back(type);
        }

        std::array<int64_t, 3> specShape{static_cast<int64_t>(InworldStepFrames), static_cast<int64_t>(InworldBins), 2};
        std::array<int64_t, 4> maskShape{1, 1, static_cast<int64_t>(InworldStepFrames), static_cast<int64_t>(InworldAttnWindow)};
        std::array<int64_t, 3> rawShape{static_cast<int64_t>(InworldStepFrames), static_cast<int64_t>(InworldBins), 4};
        inworldSpecTensor = Ort::Value::CreateTensor<float>(memory, inworldSpecBuf.data(), inworldSpecBuf.size(), specShape.data(), specShape.size());
        inworldMaskTensor = Ort::Value::CreateTensor<Ort::Float16_t>(memory, inworldMaskBuf.data(), inworldMaskBuf.size(), maskShape.data(), maskShape.size());
        inworldRawTensor = Ort::Value::CreateTensor<float>(memory, inworldRawBuf.data(), inworldRawBuf.size(), rawShape.data(), rawShape.size());

        // Pre-build 3 persistent IoBindings:
        // - initBinding: CPU zero states -> GPU bank 0 (used once on frame 0 after reset)
        // - bindings[0]: GPU bank 0 -> GPU bank 1
        // - bindings[1]: GPU bank 1 -> GPU bank 0
        auto initBind = std::make_unique<Ort::IoBinding>(*session);
        initBind->BindInput(inNames[0].c_str(), inworldSpecTensor);
        initBind->BindInput(inNames[1].c_str(), inworldMaskTensor);
        for (size_t i = 0; i < 14; ++i) initBind->BindInput(inNames[i + 2].c_str(), zeroTensors[i]);
        initBind->BindOutput(outNames[0].c_str(), inworldRawTensor);
        for (size_t i = 0; i < 14; ++i) initBind->BindOutput(outNames[i + 1].c_str(), gpuStates[0][i]);

        std::array<std::unique_ptr<Ort::IoBinding>, 2> binds;
        for (int p = 0; p < 2; ++p) {
            binds[p] = std::make_unique<Ort::IoBinding>(*session);
            binds[p]->BindInput(inNames[0].c_str(), inworldSpecTensor);
            binds[p]->BindInput(inNames[1].c_str(), inworldMaskTensor);
            for (size_t i = 0; i < 14; ++i) binds[p]->BindInput(inNames[i + 2].c_str(), gpuStates[p][i]);
            binds[p]->BindOutput(outNames[0].c_str(), inworldRawTensor);
            for (size_t i = 0; i < 14; ++i) binds[p]->BindOutput(outNames[i + 1].c_str(), gpuStates[1 - p][i]);
        }

        inworldDmlMem = std::move(dmlMem);
        inworldDmlAlloc = std::move(dmlAlloc);
        inworldInputNames = std::move(inNames);
        inworldOutputNames = std::move(outNames);
        inworldStateShapes = std::move(stateShapes);
        inworldStateTypes = std::move(stateTypes);
        inworldZeroFp16 = std::move(zero16);
        inworldZeroFp32 = std::move(zero32);
        inworldZeroTensors = std::move(zeroTensors);
        inworldGpuStates = std::move(gpuStates);
        inworldInitBinding = std::move(initBind);
        inworldBindings = std::move(binds);
        inworld = std::move(session);
        inworldDeviceId = deviceId;
        resetInworldState();
        printf("[VOICE] Loaded Inworld Realtime on GPU %d (%s)\n", deviceId, gpuDeviceName(deviceId).c_str());
#endif
    }

    void resetDfnState() {
        dfnStates.clear();
        if (!dfn) return;
        for (size_t i = 1; i < dfnShapes.size(); ++i) {
            size_t count = 1;
            for (auto d : dfnShapes[i]) count *= static_cast<size_t>(d);
            dfnStates.emplace_back(count, 0.f);
            auto& state = dfnStates.back();
            if (dfnInputNames[i] == "erb_norm_state")
                for (size_t j = 0; j < count; ++j) state[j] = -60.f - 30.f * j / (count - 1);
            if (dfnInputNames[i] == "band_unit_norm_state")
                for (size_t j = 0; j < count; ++j) state[j] = .001f - .0009f * j / (count - 1);
        }
    }

    void resetFastEnhancerState() {
        feStates.clear();
        if (!fe) return;
        for (size_t i = 1; i < feShapes.size(); ++i) {
            size_t count = 1;
            for (auto d : feShapes[i]) count *= static_cast<size_t>(d);
            feStates.emplace_back(count, 0.f);
        }
    }

    void resetDpdfState() {
        if (dpdf) dpdfState = dpdfInitState;
        dpdfAnalysis.fill(0.f);
        dpdfOla.fill(0.f);
    }

    void resetInworldState() {
        inworldPing = 0;
        inworldAnalysis.fill(0.f);
        inworldOla.fill(0.f);
        inworldEnv.fill(0.f);
        inworldSpecQueue.clear();
        std::array<float, InworldBins * 2> zeroSpec{};
        for (size_t i = 0; i < InworldLookahead; ++i) inworldSpecQueue.push_back(zeroSpec);
        inworldDry.assign(InworldDelay, 0.f);
        inworldFrameCount = 0;
    }

    void reset(size_t delaySamples) {
        currentDelay = delaySamples;
        resetDfnState();
        resetFastEnhancerState();
        resetDpdfState();
        resetInworldState();
        pending.clear();
        dry.assign(delaySamples, 0.f);
        vadPending.clear();
        vadState.fill(0); context.fill(0);
        gain = 1; limiterGain = 1; sinceSpeech = 0; speech = true; z1 = z2 = 0;
    }

    void reset() { reset(currentDelay); }

    float probability(const float* window) {
        std::array<float,576> x;
        std::copy(context.begin(),context.end(),x.begin());
        std::copy(window,window+512,x.begin()+64);
        std::array<int64_t,2> xs{1,576};
        std::array<int64_t,3> ss{2,1,128};
        int64_t sr=16000;
        std::vector<Ort::Value> values;
        values.push_back(Ort::Value::CreateTensor<float>(memory,x.data(),x.size(),xs.data(),xs.size()));
        values.push_back(Ort::Value::CreateTensor<float>(memory,vadState.data(),vadState.size(),ss.data(),ss.size()));
        values.push_back(Ort::Value::CreateTensor<int64_t>(memory,&sr,1,nullptr,0));
        const char* names[]={"input","state","sr"}; const char* outs[]={"output","stateN"};
        auto result=vad.Run(Ort::RunOptions{nullptr},names,values.data(),3,outs,2);
        std::copy_n(result[1].GetTensorData<float>(),256,vadState.begin());
        std::copy(x.end()-64,x.end(),context.begin());
        return result[0].GetTensorData<float>()[0];
    }

    std::vector<float> runInworld(const std::vector<float>& frame) {
        const auto& fft = rfft2048();
        for (size_t t = 0; t < InworldStepFrames; ++t) {
            std::copy(inworldAnalysis.begin() + InworldHop, inworldAnalysis.end(), inworldAnalysis.begin());
            std::copy_n(frame.data() + t * InworldHop, InworldHop, inworldAnalysis.begin() + (InworldWin - InworldHop));

            std::array<float, InworldWin> windowed{};
            for (size_t i = 0; i < InworldWin; ++i) windowed[i] = inworldAnalysis[i] * fft.hann[i];
            std::array<float, InworldBins * 2> frameSpec{};
            fft.rfft(windowed.data(), frameSpec.data());
            std::copy(frameSpec.begin(), frameSpec.end(), inworldSpecBuf.begin() + t * InworldBins * 2);
            inworldSpecQueue.push_back(frameSpec);

            // Row t corresponds to frame (inworldFrameCount + t) placed at cache slot
            // (InworldAttnWindow - InworldStepFrames + t). Future slots after it are masked.
            const size_t cacheEnd = InworldAttnWindow - InworldStepFrames + t + 1;
            const size_t valid = std::min(inworldFrameCount + t + 1, cacheEnd);
            const size_t maskedLead = cacheEnd - valid;
            auto* maskRow = inworldMaskBuf.data() + t * InworldAttnWindow;
            for (size_t i = 0; i < maskedLead; ++i) maskRow[i] = Ort::Float16_t::FromBits(Fp16Neg10000);
            for (size_t i = maskedLead; i < cacheEnd; ++i) maskRow[i] = Ort::Float16_t::FromBits(Fp16Zero);
            for (size_t i = cacheEnd; i < InworldAttnWindow; ++i) maskRow[i] = Ort::Float16_t::FromBits(Fp16Neg10000);
        }

        Ort::IoBinding* activeBinding = nullptr;
        if (inworldFrameCount == 0) {
            activeBinding = inworldInitBinding.get();
            inworldPing = 0; // initBinding writes to GPU bank 0 -> next step reads bank 0
        } else {
            activeBinding = inworldBindings[inworldPing].get();
            inworldPing ^= 1;
        }
        activeBinding->BindInput(inworldInputNames[0].c_str(), inworldSpecTensor);
        activeBinding->BindInput(inworldInputNames[1].c_str(), inworldMaskTensor);
        inworld->Run(Ort::RunOptions{nullptr}, *activeBinding);
        activeBinding->SynchronizeOutputs();

        std::vector<float> out(InworldBatchHop);
        for (size_t t = 0; t < InworldStepFrames; ++t) {
            const float* raw = inworldRawBuf.data() + t * InworldBins * 4;
            const auto delayedSpec = inworldSpecQueue.front();
            inworldSpecQueue.pop_front();
            if (inworldFrameCount >= InworldLookahead) {
                std::array<float, InworldBins * 2> deltaSpec{};
                for (size_t b = 0; b < InworldBins; ++b) {
                    const float spRe = delayedSpec[2 * b], spIm = delayedSpec[2 * b + 1];
                    const float r0 = raw[4 * b], r1 = raw[4 * b + 1], r2 = raw[4 * b + 2], r3 = raw[4 * b + 3];
                    const float mRe = 2.f * std::tanh(r0), mIm = 2.f * std::tanh(r1);
                    const float resRe = std::tanh(r2), resIm = std::tanh(r3);
                    const float nrm = std::max(std::sqrt(resRe * resRe + resIm * resIm), 1.f);
                    const float scale = 0.5f * std::sqrt(spRe * spRe + spIm * spIm) / nrm;
                    deltaSpec[2 * b] = spRe * mRe - spIm * mIm + resRe * scale;
                    deltaSpec[2 * b + 1] = spRe * mIm + spIm * mRe + resIm * scale;
                }
                std::array<float, InworldWin> synth{};
                fft.irfft(deltaSpec.data(), synth.data());
                for (size_t i = 0; i < InworldWin; ++i) {
                    inworldOla[i] += synth[i] * fft.hann[i];
                    inworldEnv[i] += fft.hannSq[i];
                }
            }
            ++inworldFrameCount;

            const float* drySrc = frame.data() + t * InworldHop;
            inworldDry.insert(inworldDry.end(), drySrc, drySrc + InworldHop);
            for (size_t i = 0; i < InworldHop; ++i) {
                const float delta = inworldEnv[i] > 1e-11f ? (inworldOla[i] / inworldEnv[i]) : 0.f;
                out[t * InworldHop + i] = inworldDry[i] + delta;
            }
            inworldDry.erase(inworldDry.begin(), inworldDry.begin() + InworldHop);
            std::copy(inworldOla.begin() + InworldHop, inworldOla.end(), inworldOla.begin());
            std::fill(inworldOla.begin() + (InworldWin - InworldHop), inworldOla.end(), 0.f);
            std::copy(inworldEnv.begin() + InworldHop, inworldEnv.end(), inworldEnv.begin());
            std::fill(inworldEnv.begin() + (InworldWin - InworldHop), inworldEnv.end(), 0.f);
        }
        return out;
    }

    std::vector<float> runDenoiser(const std::vector<float>& frame, int model) {
        if (model == VoiceSettings::ModelInworldGpu) return runInworld(frame);
        if (model == VoiceSettings::ModelFastEnhancerL) {
            std::vector<float> in(frame);
            std::vector<Ort::Value> values;
            values.reserve(feShapes.size());
            values.push_back(Ort::Value::CreateTensor<float>(memory, in.data(), in.size(), feShapes[0].data(), feShapes[0].size()));
            for (size_t i = 0; i < feStates.size(); ++i)
                values.push_back(Ort::Value::CreateTensor<float>(memory, feStates[i].data(), feStates[i].size(), feShapes[i+1].data(), feShapes[i+1].size()));
            auto result = fe->Run(Ort::RunOptions{nullptr}, feInputs.data(), values.data(), values.size(), feOutputs.data(), feOutputs.size());
            std::vector<float> out(FastEnhancerHop);
            std::copy_n(result[0].GetTensorData<float>(), FastEnhancerHop, out.begin());
            for (size_t i = 0; i < feStates.size(); ++i)
                std::copy_n(result[i+1].GetTensorData<float>(), feStates[i].size(), feStates[i].begin());
            return out;
        }
        if (model == VoiceSettings::ModelDpdfNet8) {
            const auto& fft = rfft960();
            std::copy(dpdfAnalysis.begin() + DpdfHop, dpdfAnalysis.end(), dpdfAnalysis.begin());
            std::copy_n(frame.data(), DpdfHop, dpdfAnalysis.begin() + DpdfHop);
            std::array<float, DpdfWin> windowed{};
            for (size_t i = 0; i < DpdfWin; ++i) windowed[i] = dpdfAnalysis[i] * fft.vorbis[i];
            std::array<float, DpdfBins * 2> specIn{};
            fft.rfft(windowed.data(), specIn.data());

            std::array<int64_t, 4> specShape{1, 1, static_cast<int64_t>(DpdfBins), 2};
            std::array<int64_t, 1> stateShape{static_cast<int64_t>(dpdfState.size())};
            std::vector<Ort::Value> values;
            values.reserve(2);
            values.push_back(Ort::Value::CreateTensor<float>(memory, specIn.data(), specIn.size(), specShape.data(), specShape.size()));
            values.push_back(Ort::Value::CreateTensor<float>(memory, dpdfState.data(), dpdfState.size(), stateShape.data(), stateShape.size()));
            const char* inNames[] = {dpdfSpecInName.c_str(), dpdfStateInName.c_str()};
            const char* outNames[] = {dpdfSpecOutName.c_str(), dpdfStateOutName.c_str()};
            auto result = dpdf->Run(Ort::RunOptions{nullptr}, inNames, values.data(), 2, outNames, 2);
            std::copy_n(result[1].GetTensorData<float>(), dpdfState.size(), dpdfState.begin());

            std::array<float, DpdfWin> synth{};
            fft.irfft(result[0].GetTensorData<float>(), synth.data());
            std::copy(dpdfOla.begin() + DpdfHop, dpdfOla.end(), dpdfOla.begin());
            std::fill(dpdfOla.begin() + DpdfHop, dpdfOla.end(), 0.f);
            for (size_t i = 0; i < DpdfWin; ++i) dpdfOla[i] += synth[i] * fft.vorbis[i];
            return {dpdfOla.begin(), dpdfOla.begin() + DpdfHop};
        }
        std::vector<float> in(frame);
        std::vector<Ort::Value> values;
        values.reserve(dfnShapes.size());
        values.push_back(Ort::Value::CreateTensor<float>(memory, in.data(), in.size(), dfnShapes[0].data(), dfnShapes[0].size()));
        for (size_t i = 0; i < dfnStates.size(); ++i)
            values.push_back(Ort::Value::CreateTensor<float>(memory, dfnStates[i].data(), dfnStates[i].size(), dfnShapes[i+1].data(), dfnShapes[i+1].size()));
        auto result = dfn->Run(Ort::RunOptions{nullptr}, dfnInputs.data(), values.data(), values.size(), dfnOutputs.data(), dfnOutputs.size());
        std::vector<float> out(DfnHop);
        std::copy_n(result[0].GetTensorData<float>(), DfnHop, out.begin());
        for (size_t i = 0; i < dfnStates.size(); ++i)
            std::copy_n(result[i+1].GetTensorData<float>(), dfnStates[i].size(), dfnStates[i].begin());
        return out;
    }

    std::vector<float> hop(const float* block, size_t hopSize, VoiceSettings s) {
        std::vector<float> frame(block, block + hopSize);
        if (s.lowCut) {
            const double w=2*Pi*80/Rate, a=std::sin(w)/std::sqrt(2.), c=std::cos(w), a0=1+a;
            const double b0=(1+c)/2/a0, b1=-(1+c)/a0, a1=-2*c/a0, a2=(1-a)/a0;
            for (float& x : frame) { double y=b0*x+z1; z1=b1*x-a1*y+z2; z2=b0*x-a2*y; x=static_cast<float>(y); }
        } else z1=z2=0;
        auto processed=frame;
        if (s.noise) {
            processed = runDenoiser(frame, s.model);
            const float mix=s.strength>=100 ? 0.f : std::pow(10.f,-s.strength/20.f);
            dry.insert(dry.end(), frame.begin(), frame.end());
            for (size_t i=0;i<hopSize;++i) processed[i]=(1-mix)*processed[i]+mix*dry[i];
            dry.erase(dry.begin(), dry.begin() + hopSize);
        }
        if (s.gate) {
            // Preserve groups of three across hops when converting 48 kHz to 16 kHz.
            vadPending.insert(vadPending.end(),processed.begin(),processed.end());
            while (vadPending.size()>=1536) {
                std::array<float,512> down;
                for (size_t i=0;i<512;++i) down[i]=(vadPending[3*i]+vadPending[3*i+1]+vadPending[3*i+2])/3.f;
                bool active=probability(down.data())>=s.threshold/100.f;
                sinceSpeech=active ? 0 : sinceSpeech+1536;
                if (active) speech=true;
                else if (sinceSpeech>static_cast<size_t>(s.holdMs)*48) speech=false;
                vadPending.erase(vadPending.begin(),vadPending.begin()+1536);
            }
        } else { speech=true; sinceSpeech=0; vadPending.clear(); }
        // Makeup gain restores level lost to suppression. When boosting, a peak limiter
        // (instant attack, ~80 ms release) holds peaks at -1 dBFS instead of clipping them;
        // softLimit is only a safety net. Without a boost, output is unchanged.
        const float makeup = s.noise && s.strength > 0 ? std::pow(10.f, s.makeupDb / 20.f) : 1.f;
        constexpr float ceiling = .89f, release = 1.f / (.08f * 48000);
        for (float& x : processed) {
            gain=std::clamp(gain+(speech ? 1.f/240 : -1.f/3840),0.f,1.f);
            if (!std::isfinite(x)) { x = 0.f; continue; }
            if (makeup <= 1.f) { x = std::clamp(x*gain,-1.f,1.f); continue; }
            const float y = x*gain*makeup, level = std::abs(y);
            const float needed = level > ceiling ? ceiling / level : 1.f;
            limiterGain = std::min(needed, limiterGain + (1.f - limiterGain) * release);
            x = softLimit(y * limiterGain);
        }
        return processed;
    }
};

VoiceDsp::VoiceDsp(const std::wstring& dir, int initialModel, int initialGpu)
    : impl_(std::make_unique<Impl>(dir, initialModel, initialGpu)) {}
VoiceDsp::~VoiceDsp()=default;
std::vector<VoiceGpuDevice> VoiceDsp::enumerateGpuDevices() {
#ifdef AUDIOBRIDGE_HAS_DIRECTML
    return listDmlDevices();
#else
    return {};
#endif
}
void VoiceDsp::reset() { impl_->reset(); }
void VoiceDsp::releaseGpu() { impl_->releaseInworld(); }
int VoiceDsp::activeGpuDevice() const { return impl_->inworldDeviceId; }
std::vector<float> VoiceDsp::process(const float* samples,size_t count,VoiceSettings s) {
    auto& p=*impl_;
    s.model = std::clamp(s.model, 0, VoiceSettings::ModelCount - 1);
    const size_t hopSize = s.noise ? modelHop(s.model) : DefaultHop;
    const size_t delaySamples = s.noise ? modelDelay(s.model) : 0;
    if (s.noise) p.ensureModel(s.model, s.gpuDevice);
    if (!p.haveSettings) {
        if (p.currentDelay != delaySamples) p.reset(delaySamples);
    } else if (p.previous.noise!=s.noise || p.previous.gate!=s.gate ||
               (s.noise && (p.previous.model!=s.model || (s.model==VoiceSettings::ModelInworldGpu && p.previous.gpuDevice!=s.gpuDevice)))) {
        p.reset(delaySamples);
    }
    p.previous=s; p.haveSettings=true;
    p.pending.insert(p.pending.end(),samples,samples+count);
    std::vector<float> out;
    size_t consumed=0;
    while (p.pending.size()-consumed>=hopSize) {
        auto block=p.hop(p.pending.data()+consumed,hopSize,s);
        out.insert(out.end(),block.begin(),block.end()); consumed+=hopSize;
    }
    p.pending.erase(p.pending.begin(),p.pending.begin()+consumed);
    return out;
}
std::vector<float> VoiceDsp::render(const std::vector<float>& clip,VoiceSettings s) {
    if (clip.empty()) return {};
    s.model = std::clamp(s.model, 0, VoiceSettings::ModelCount - 1);
    const size_t hopSize = s.noise ? modelHop(s.model) : DefaultHop;
    const size_t shift = s.noise ? modelDelay(s.model) : 0;
    if (s.noise) impl_->ensureModel(s.model, s.gpuDevice);
    impl_->reset(shift);
    std::vector<float> padded=clip;
    padded.resize(((clip.size()+shift+hopSize-1)/hopSize)*hopSize,0.f);
    auto result=process(padded.data(),padded.size(),s);
    return {result.begin()+shift,result.begin()+shift+clip.size()};
}

VoiceEffects::VoiceEffects(const std::wstring& dir,VoiceSettings s,Sink sink)
    : modelDir_(dir),sink_(std::move(sink)),settings_(s),gpuDevices_(VoiceDsp::enumerateGpuDevices()),
      worker_(&VoiceEffects::run,this),previewWorker_(&VoiceEffects::previewRun,this) {}
VoiceEffects::~VoiceEffects() { stop(); }
void VoiceEffects::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); running_=false; }
    wake_.notify_all(); previewWake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (previewWorker_.joinable()) previewWorker_.join();
}
void VoiceEffects::push(const float* samples,unsigned count) {
    if (!samples || !count) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    if (recording_) {
        size_t take=std::min<size_t>(count,MaxClip-original_.size());
        original_.insert(original_.end(),samples,samples+take);
        if (original_.size()==MaxClip) { recording_=false; requestPreview(); }
    }
    if (queue_.size()+count>4800) {
        queue_.clear(); hasQueuedPush_ = false; reset_=true; ++streamGeneration_; ++dropped_;
        if (ready_) {
            lastOverflowAt_ = std::chrono::steady_clock::now();
            hasOverflow_ = true;
        }
        if (dropped_ <= 5 || (dropped_ % 50) == 0)
            printf("[VOICE] Queue overflow #%u (dropped stale audio, resetting DSP)\n", dropped_);
    }
    if (count<=4800) {
        if (queue_.empty() || !hasQueuedPush_) {
            oldestPushAt_ = std::chrono::steady_clock::now();
            hasQueuedPush_ = true;
        }
        queue_.insert(queue_.end(),samples,samples+count);
    }
    wake_.notify_one();
}
void VoiceEffects::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear(); hasQueuedPush_ = false; reset_=true; ++streamGeneration_; wake_.notify_one();
    // End a recording at ownership handoff so unrelated phones are never combined.
    if (recording_) { recording_=false; requestPreview(); }
}
void VoiceEffects::requestPreview() {
    ++generation_; processed_.clear();
    renderPending_=!original_.empty(); previewError_.clear();
    previewWake_.notify_one();
}
void VoiceEffects::setSettings(VoiceSettings s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (settings_==s) return;
    const bool backendChanged = settings_.model != s.model || settings_.gpuDevice != s.gpuDevice || settings_.noise != s.noise;
    settings_=s;
    if (backendChanged) {
        hasOverflow_ = false;
        rtf_ = 0.0;
        wake_.notify_one();
    }
    if (!recording_) requestPreview();
}
VoiceSettings VoiceEffects::settings() const { std::lock_guard<std::mutex> lock(mutex_); return settings_; }
void VoiceEffects::record(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled) { ++generation_; renderPending_=false; original_.clear(); processed_.clear(); previewError_.clear(); ++clipRevision_; }
    recording_=enabled;
    if (!enabled) requestPreview();
}
void VoiceEffects::deliver(const std::vector<float>& samples) {
    for (size_t offset = 0; offset < samples.size(); offset += DefaultHop) {
        const auto frames = static_cast<unsigned>(std::min(DefaultHop, samples.size() - offset));
        sink_(samples.data() + offset, frames);
    }
}
void VoiceEffects::run() {
    std::unique_ptr<VoiceDsp> dsp;
    int loadedModel = -1;
    int loadedGpu = -2;
    int failedModel = -1;
    int failedGpu = -2;
    try {
        VoiceSettings initSettings;
        { std::lock_guard<std::mutex> lock(mutex_); initSettings = settings_; }
        dsp = std::make_unique<VoiceDsp>(modelDir_, -1, -1);
        try {
            if (initSettings.noise) {
                dsp->process(nullptr, 0, initSettings);
                loadedModel = initSettings.model;
                loadedGpu = initSettings.gpuDevice;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = true;
            recoverable_ = false;
            activeGpu_ = dsp->activeGpuDevice();
            printf("[VOICE] Initialized model=%d gpu=%d (activeGpu=%d)\n", initSettings.model, initSettings.gpuDevice, activeGpu_);
        } catch (const std::exception& e) {
            dsp->releaseGpu();
            failedModel = initSettings.model;
            failedGpu = initSettings.gpuDevice;
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = e.what();
            ready_ = false;
            recoverable_ = true;
            activeGpu_ = -1;
            printf("[VOICE] Model init error (recoverable): %s\n", error_.c_str());
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = e.what();
        ready_ = false;
        recoverable_ = false;
        printf("[VOICE] Init error: %s\n", error_.c_str());
    }
    auto lastStats = std::chrono::steady_clock::now();
    uint64_t statBlocks = 0;
    uint64_t statSamples = 0;
    double statTotalMs = 0.0, statMaxMs = 0.0;
    bool hasPendingStart = false;
    std::chrono::steady_clock::time_point pendingStartAt{};
    auto needsReload = [&](const VoiceSettings& cfg) {
        if (!dsp) return false;
        if (!cfg.noise) return failedModel >= 0;
        if (cfg.model == failedModel && cfg.gpuDevice == failedGpu) return false;
        if (failedModel >= 0) return true;
        if (cfg.model != loadedModel) return true;
        return cfg.model == VoiceSettings::ModelInworldGpu && cfg.gpuDevice != loadedGpu;
    };
    for (;;) {
        std::vector<float> block; VoiceSettings s; bool reset; unsigned streamGeneration;
        std::chrono::steady_clock::time_point pushAt{};
        bool havePushAt = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock,[&]{return !running_ || !queue_.empty() || reset_ || needsReload(settings_);});
            if (!running_) break;
            block.assign(queue_.begin(),queue_.end()); queue_.clear();
            pushAt = oldestPushAt_; havePushAt = hasQueuedPush_; hasQueuedPush_ = false;
            s=settings_; reset=reset_; reset_=false;
            streamGeneration=streamGeneration_;
        }
        if (reset) hasPendingStart = false;
        const bool isFailedBackend = s.noise && s.model == failedModel && s.gpuDevice == failedGpu;
        if (block.empty()) {
            try {
                if (dsp && !isFailedBackend) {
                    if (needsReload(s)) {
                        if (s.noise) {
                            dsp->process(nullptr, 0, s);
                            loadedModel = s.model;
                            loadedGpu = s.gpuDevice;
                        }
                        failedModel = -1;
                        failedGpu = -2;
                        std::lock_guard<std::mutex> lock(mutex_);
                        error_.clear();
                        ready_ = true;
                        recoverable_ = false;
                        activeGpu_ = dsp->activeGpuDevice();
                        printf("[VOICE] Switched to model=%d gpu=%d (activeGpu=%d)\n", s.model, s.gpuDevice, activeGpu_);
                    }
                    if (reset) dsp->reset();
                }
            } catch (const std::exception& e) {
                dsp->releaseGpu();
                failedModel = s.model;
                failedGpu = s.gpuDevice;
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = e.what();
                ready_ = false;
                recoverable_ = true;
                activeGpu_ = -1;
                printf("[VOICE] Model/GPU switch error: %s\n", error_.c_str());
            }
            continue;
        }
        try {
            if (dsp && !isFailedBackend) {
                if (reset) dsp->reset();
                if (!hasPendingStart && havePushAt) {
                    pendingStartAt = pushAt;
                    hasPendingStart = true;
                }
                const auto t0 = std::chrono::steady_clock::now();
                auto out=dsp->process(block.data(),block.size(),s);
                const auto t1 = std::chrono::steady_clock::now();
                const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (s.noise) { loadedModel = s.model; loadedGpu = s.gpuDevice; }
                if (failedModel >= 0) {
                    failedModel = -1; failedGpu = -2;
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_.clear(); ready_ = true; recoverable_ = false;
                }
                if (!out.empty()) {
                    const double pipeMs = hasPendingStart
                        ? std::chrono::duration<double, std::milli>(t1 - pendingStartAt).count()
                        : elapsedMs;
                    hasPendingStart = false;
                    const double delayMs = s.noise ? (static_cast<double>(modelDelay(s.model)) * 1000.0 / static_cast<double>(Rate)) : 0.0;
                    const double outAudioMs = static_cast<double>(out.size()) * 1000.0 / static_cast<double>(Rate);
                    const double sampleRtf = outAudioMs > 0.0 ? (elapsedMs / outAudioMs) : 0.0;
                    std::lock_guard<std::mutex> lock(mutex_);
                    activeGpu_ = dsp->activeGpuDevice();
                    modelDelayMs_ = delayMs;
                    if (!hasProcessed_) {
                        processMs_ = elapsedMs;
                        pipelineMs_ = pipeMs;
                        rtf_ = sampleRtf;
                    } else {
                        processMs_ = processMs_ * 0.8 + elapsedMs * 0.2;
                        pipelineMs_ = pipelineMs_ * 0.8 + pipeMs * 0.2;
                        rtf_ = rtf_ * 0.8 + sampleRtf * 0.2;
                    }
                    lastProcessedAt_ = t1;
                    hasProcessed_ = true;
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    activeGpu_ = dsp->activeGpuDevice();
                }
                ++statBlocks;
                statSamples += block.size();
                statTotalMs += elapsedMs;
                if (elapsedMs > statMaxMs) statMaxMs = elapsedMs;
                if (std::chrono::duration_cast<std::chrono::seconds>(t1 - lastStats).count() >= 5 && statBlocks > 0) {
                    unsigned droppedNow = 0;
                    { std::lock_guard<std::mutex> lock(mutex_); droppedNow = dropped_; }
                    const double audioMs = static_cast<double>(statSamples) * 1000.0 / 48000.0;
                    printf("[VOICE] Stats: model=%d gpu=%d blocks=%llu avg=%.2fms max=%.2fms rtf=%.3fx dropped=%u\n",
                           s.noise ? s.model : -1, dsp->activeGpuDevice(), statBlocks, statTotalMs / statBlocks, statMaxMs,
                           audioMs > 0.0 ? statTotalMs / audioMs : 0.0, droppedNow);
                    statBlocks = 0; statSamples = 0; statTotalMs = 0.0; statMaxMs = 0.0;
                    lastStats = t1;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                if (running_ && streamGeneration==streamGeneration_ && !out.empty())
                    deliver(out);
            } else {
                const auto t1 = std::chrono::steady_clock::now();
                const double pipeMs = havePushAt ? std::chrono::duration<double, std::milli>(t1 - pushAt).count() : 0.0;
                hasPendingStart = false;
                std::lock_guard<std::mutex> lock(mutex_);
                modelDelayMs_ = 0.0;
                processMs_ = 0.0;
                pipelineMs_ = hasProcessed_ ? (pipelineMs_ * 0.8 + pipeMs * 0.2) : pipeMs;
                lastProcessedAt_ = t1;
                hasProcessed_ = true;
                if (running_ && streamGeneration==streamGeneration_)
                    deliver(block);
            }
        } catch (const std::exception& e) {
            if (dsp) dsp->releaseGpu();
            hasPendingStart = false;
            failedModel = s.model;
            failedGpu = s.gpuDevice;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = e.what();
                ready_ = false;
                recoverable_ = (dsp != nullptr);
                activeGpu_ = -1;
                printf("[VOICE] Process error: %s\n", error_.c_str());
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ && streamGeneration==streamGeneration_)
                deliver(block);
        }
    }
}
void VoiceEffects::previewRun() {
    std::unique_ptr<VoiceDsp> dsp;
    for (;;) {
        std::vector<float> clip; VoiceSettings s; unsigned generation;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            previewWake_.wait(lock,[&]{return !running_ || renderPending_;});
            if (!running_) break;
            clip=original_; s=settings_; generation=generation_; renderPending_=false; rendering_=true;
        }
        try {
            if (!dsp) dsp=std::make_unique<VoiceDsp>(modelDir_, s.model, s.gpuDevice);
            auto out=dsp->render(clip,s);
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation==generation_) { processed_=std::move(out); ++clipRevision_; }
        } catch (const std::exception& e) {
            if (dsp) dsp->releaseGpu();
            std::lock_guard<std::mutex> lock(mutex_); if (generation==generation_) previewError_=e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_); rendering_=false;
    }
}
std::string VoiceEffects::stateJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string gpus = "[";
    for (size_t i = 0; i < gpuDevices_.size(); ++i) {
        if (i) gpus += ",";
        gpus += "{\"id\":" + std::to_string(gpuDevices_[i].id) +
            ",\"name\":" + quote(gpuDevices_[i].name) +
            ",\"vramMb\":" + std::to_string(gpuDevices_[i].vramMb) + "}";
    }
    gpus += "]";
    const auto now = std::chrono::steady_clock::now();
    const bool active = hasProcessed_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProcessedAt_).count() < 1500;
    const bool recentOverflow = hasOverflow_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastOverflowAt_).count() < 3000;
    const bool overloaded = ready_ && settings_.noise && (recentOverflow || (active && rtf_ > 1.15));
    char latBuf[220];
    snprintf(latBuf, sizeof(latBuf), ",\"active\":%s,\"overloaded\":%s,\"rtf\":%.2f,\"modelDelayMs\":%.1f,\"processMs\":%.1f,\"pipelineMs\":%.1f",
             active ? "true" : "false", overloaded ? "true" : "false", rtf_, modelDelayMs_, processMs_, pipelineMs_);
    return "{\"settings\":"+quote(settings_.serialize())+",\"ready\":"+(ready_?"true":"false")+
        ",\"recoverable\":"+(recoverable_?"true":"false")+
        ",\"activeGpu\":"+std::to_string(activeGpu_)+",\"gpuDevices\":"+gpus+std::string(latBuf)+
        ",\"error\":"+quote(error_)+",\"previewError\":"+quote(previewError_)+
        ",\"recording\":"+(recording_?"true":"false")+",\"rendering\":"+((rendering_||renderPending_)?"true":"false")+
        ",\"duration\":"+std::to_string(original_.size()/48000.)+",\"processed\":"+(!processed_.empty()?"true":"false")+
        ",\"revision\":"+std::to_string(clipRevision_)+",\"dropped\":"+std::to_string(dropped_)+"}";
}
std::string VoiceEffects::clipJson(bool processed) const {
    std::vector<float> clip;
    { std::lock_guard<std::mutex> lock(mutex_); if (recording_) return "null"; clip=processed?processed_:original_; }
    return clip.empty()?"null":quote(wav(clip));
}
