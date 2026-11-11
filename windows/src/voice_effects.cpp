#include "voice_effects.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace {
constexpr size_t Hop = 512, Delay = 1536, Rate = 48000, MaxClip = 10 * Rate;
Ort::Env& environment() { static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "AudioBridge voice"); return env; }
Ort::SessionOptions sessionOptions() {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return options;
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
    return s;
}
std::string VoiceSettings::serialize() const {
    return std::to_string(noise) + " " + std::to_string(strength) + " " +
        std::to_string(gate) + " " + std::to_string(threshold) + " " +
        std::to_string(holdMs) + " " + std::to_string(lowCut);
}

struct VoiceDsp::Impl {
    Ort::Session dfn, vad;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<std::string> inputNames, outputNames;
    std::vector<const char*> inputs, outputs;
    std::vector<std::vector<int64_t>> shapes;
    std::vector<std::vector<float>> states;
    std::vector<float> pending, dry, vadPending;
    std::array<float,256> vadState{};
    std::array<float,64> context{};
    float gain = 1.f;
    size_t sinceSpeech = 0;
    bool speech = true;
    double z1 = 0, z2 = 0;
    VoiceSettings previous;
    bool haveSettings = false;

    explicit Impl(const std::wstring& dir)
        : dfn(environment(), (std::filesystem::path(dir)/L"deepfilternet3_streaming.onnx").c_str(), sessionOptions()),
          vad(environment(), (std::filesystem::path(dir)/L"silero_vad.onnx").c_str(), sessionOptions()) {
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i=0;i<dfn.GetInputCount();++i) {
            inputNames.emplace_back(dfn.GetInputNameAllocated(i,allocator).get());
            shapes.push_back(dfn.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        }
        for (size_t i=0;i<dfn.GetOutputCount();++i)
            outputNames.emplace_back(dfn.GetOutputNameAllocated(i,allocator).get());
        if (inputNames.empty() || inputNames[0] != "input_frame" || outputNames.size() != inputNames.size())
            throw std::runtime_error("Unsupported DeepFilterNet model signature");
        for (const auto& n : inputNames) inputs.push_back(n.c_str());
        for (const auto& n : outputNames) outputs.push_back(n.c_str());
        reset();
    }
    void reset() {
        states.clear();
        for (size_t i=1;i<shapes.size();++i) {
            size_t count = 1;
            for (auto d : shapes[i]) {
                if (d <= 0) throw std::runtime_error("Dynamic DeepFilterNet state shape");
                count *= static_cast<size_t>(d);
            }
            states.emplace_back(count,0.f);
            auto& state = states.back();
            if (inputNames[i] == "erb_norm_state")
                for (size_t j=0;j<count;++j) state[j] = -60.f - 30.f*j/(count-1);
            if (inputNames[i] == "band_unit_norm_state")
                for (size_t j=0;j<count;++j) state[j] = .001f - .0009f*j/(count-1);
        }
        pending.clear(); dry.assign(Delay,0.f); vadPending.clear();
        vadState.fill(0); context.fill(0); gain = 1; sinceSpeech = 0; speech = true; z1 = z2 = 0;
    }
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
    std::vector<float> hop(const float* block, VoiceSettings s) {
        std::vector<float> frame(block,block+Hop);
        if (s.lowCut) {
            constexpr double pi=3.141592653589793;
            const double w=2*pi*80/Rate, a=std::sin(w)/std::sqrt(2.), c=std::cos(w), a0=1+a;
            const double b0=(1+c)/2/a0, b1=-(1+c)/a0, a1=-2*c/a0, a2=(1-a)/a0;
            for (float& x : frame) { double y=b0*x+z1; z1=b1*x-a1*y+z2; z2=b0*x-a2*y; x=static_cast<float>(y); }
        } else z1=z2=0;
        auto processed=frame;
        if (s.noise) {
            std::vector<Ort::Value> values;
            values.push_back(Ort::Value::CreateTensor<float>(memory,frame.data(),frame.size(),shapes[0].data(),shapes[0].size()));
            for (size_t i=0;i<states.size();++i)
                values.push_back(Ort::Value::CreateTensor<float>(memory,states[i].data(),states[i].size(),shapes[i+1].data(),shapes[i+1].size()));
            auto result=dfn.Run(Ort::RunOptions{nullptr},inputs.data(),values.data(),values.size(),outputs.data(),outputs.size());
            std::copy_n(result[0].GetTensorData<float>(),Hop,processed.begin());
            for (size_t i=0;i<states.size();++i)
                std::copy_n(result[i+1].GetTensorData<float>(),states[i].size(),states[i].begin());
            const float mix=s.strength>=100 ? 0.f : std::pow(10.f,-s.strength/20.f);
            for (size_t i=0;i<Hop;++i) processed[i]=(1-mix)*processed[i]+mix*dry[i];
            dry.erase(dry.begin(),dry.begin()+Hop); dry.insert(dry.end(),frame.begin(),frame.end());
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
        for (float& x : processed) {
            gain=std::clamp(gain+(speech ? 1.f/240 : -1.f/3840),0.f,1.f);
            x=std::clamp(std::isfinite(x) ? x*gain : 0.f,-1.f,1.f);
        }
        return processed;
    }
};
VoiceDsp::VoiceDsp(const std::wstring& dir) : impl_(std::make_unique<Impl>(dir)) {}
VoiceDsp::~VoiceDsp()=default;
void VoiceDsp::reset() { impl_->reset(); }
std::vector<float> VoiceDsp::process(const float* samples,size_t count,VoiceSettings s) {
    auto& p=*impl_;
    if (p.haveSettings && (p.previous.noise!=s.noise || p.previous.gate!=s.gate)) p.reset();
    p.previous=s; p.haveSettings=true;
    p.pending.insert(p.pending.end(),samples,samples+count);
    std::vector<float> out;
    size_t consumed=0;
    while (p.pending.size()-consumed>=Hop) {
        auto block=p.hop(p.pending.data()+consumed,s);
        out.insert(out.end(),block.begin(),block.end()); consumed+=Hop;
    }
    p.pending.erase(p.pending.begin(),p.pending.begin()+consumed);
    return out;
}
std::vector<float> VoiceDsp::render(const std::vector<float>& clip,VoiceSettings s) {
    if (clip.empty()) return {};
    reset();
    std::vector<float> padded=clip;
    const size_t shift=s.noise ? Delay : 0;
    padded.resize(((clip.size()+shift+Hop-1)/Hop)*Hop,0.f);
    auto result=process(padded.data(),padded.size(),s);
    return {result.begin()+shift,result.begin()+shift+clip.size()};
}

VoiceEffects::VoiceEffects(const std::wstring& dir,VoiceSettings s,Sink sink)
    : modelDir_(dir),sink_(std::move(sink)),settings_(s),
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
    if (queue_.size()+count>4800) { queue_.clear(); reset_=true; ++streamGeneration_; ++dropped_; }
    if (count<=4800) queue_.insert(queue_.end(),samples,samples+count);
    wake_.notify_one();
}
void VoiceEffects::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear(); reset_=true; ++streamGeneration_; wake_.notify_one();
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
    settings_=s;
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
    for (size_t offset = 0; offset < samples.size(); offset += Hop) {
        const auto frames = static_cast<unsigned>(std::min(Hop, samples.size() - offset));
        sink_(samples.data() + offset, frames);
    }
}
void VoiceEffects::run() {
    std::unique_ptr<VoiceDsp> dsp;
    try { dsp=std::make_unique<VoiceDsp>(modelDir_); std::lock_guard<std::mutex> lock(mutex_); ready_=true; }
    catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex_); error_=e.what(); }
    for (;;) {
        std::vector<float> block; VoiceSettings s; bool reset; unsigned streamGeneration;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock,[&]{return !running_ || !queue_.empty() || reset_;});
            if (!running_) break;
            block.assign(queue_.begin(),queue_.end()); queue_.clear();
            s=settings_; reset=reset_; reset_=false;
            streamGeneration=streamGeneration_;
        }
        if (block.empty()) { if (reset && dsp) dsp->reset(); continue; }
        try {
            if (dsp) {
                if (reset) dsp->reset();
                auto out=dsp->process(block.data(),block.size(),s);
                std::lock_guard<std::mutex> lock(mutex_);
                if (running_ && streamGeneration==streamGeneration_ && !out.empty())
                    deliver(out);
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                if (running_ && streamGeneration==streamGeneration_)
                    deliver(block);
            }
        } catch (const std::exception& e) {
            { std::lock_guard<std::mutex> lock(mutex_); error_=e.what(); ready_=false; }
            dsp.reset();
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
            if (!dsp) dsp=std::make_unique<VoiceDsp>(modelDir_);
            auto out=dsp->render(clip,s);
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation==generation_) { processed_=std::move(out); ++clipRevision_; }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_); if (generation==generation_) previewError_=e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_); rendering_=false;
    }
}
std::string VoiceEffects::stateJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return "{\"settings\":"+quote(settings_.serialize())+",\"ready\":"+(ready_?"true":"false")+
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
