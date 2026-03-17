#include "audio_capture.h"
#include <Functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#pragma comment(lib, "avrt.lib")

static const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
static const IID IID_IAudioClient = __uuidof(IAudioClient);
static const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stop();
    cleanup();
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
}

void AudioCapture::cleanup() {
    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    resampleBuf_.clear();
    resamplePos_ = 0.0;
    peakLevel_ = 0.0f;
    peakSampleCount_ = 0;
    sampleRate_ = 0;
    channels_ = 0;
}

std::vector<AudioDeviceInfo> AudioCapture::getDevices() {
    std::vector<AudioDeviceInfo> devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                  CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                  (void**)&enumerator);
    if (FAILED(hr)) return devices;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        enumerator->Release();
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr)) continue;

        LPWSTR deviceId = nullptr;
        hr = device->GetId(&deviceId);
        if (FAILED(hr)) {
            device->Release();
            continue;
        }

        IPropertyStore* props = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &props);
        if (FAILED(hr)) {
            CoTaskMemFree(deviceId);
            device->Release();
            continue;
        }

        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);

        AudioDeviceInfo info;
        info.id = deviceId;
        info.name = (hr == S_OK && varName.vt == VT_LPWSTR) ? varName.pwszVal : L"Unknown Device";
        devices.push_back(info);

        PropVariantClear(&varName);
        props->Release();
        CoTaskMemFree(deviceId);
        device->Release();
    }

    collection->Release();
    enumerator->Release();

    return devices;
}

bool AudioCapture::initialize(const std::wstring& deviceId) {
    if (!enumerator_) {
        HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                      CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                      (void**)&enumerator_);
        if (FAILED(hr)) {
            printf("Failed to create device enumerator: 0x%08lx\n", hr);
            return false;
        }
    }

    HRESULT hr;
    if (deviceId.empty()) {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    } else {
        hr = enumerator_->GetDevice(deviceId.c_str(), &device_);
    }
    if (FAILED(hr)) {
        printf("Failed to get audio endpoint: 0x%08lx\n", hr);
        return false;
    }

    hr = device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&audioClient_);
    if (FAILED(hr)) {
        printf("Failed to activate audio client: 0x%08lx\n", hr);
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient_->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
        printf("Failed to get mix format: 0x%08lx\n", hr);
        return false;
    }

    sampleRate_ = mixFormat->nSamplesPerSec;
    channels_ = mixFormat->nChannels;

    printf("Audio device: %u Hz, %u channels, %u bits\n",
           sampleRate_, channels_, mixFormat->wBitsPerSample);

    // Initialize in shared loopback mode with a small buffer
    REFERENCE_TIME bufferDuration = 100000; // 10ms in 100ns units
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK,
                                   bufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        printf("Failed to initialize audio client: 0x%08lx\n", hr);
        CoTaskMemFree(mixFormat);
        return false;
    }

    CoTaskMemFree(mixFormat);

    hr = audioClient_->GetService(IID_IAudioCaptureClient, (void**)&captureClient_);
    if (FAILED(hr)) {
        printf("Failed to get capture client: 0x%08lx\n", hr);
        return false;
    }

    return true;
}

bool AudioCapture::start(FrameCallback callback) {
    callback_ = callback;
    resampleBuf_.clear();
    resamplePos_ = 0.0;

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        printf("Failed to start capture: 0x%08lx\n", hr);
        return false;
    }

    running_ = true;
    thread_ = std::thread(&AudioCapture::captureThread, this);
    return true;
}

void AudioCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (audioClient_) audioClient_->Stop();
}

void AudioCapture::captureThread() {
    // Boost thread priority for low latency
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (running_) {
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                printf("Audio device invalidated\n");
                if (onDeviceInvalidated_) onDeviceInvalidated_();
            }
            break;
        }

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Deliver silence matching actual device format
                std::vector<float> silence(numFrames * channels_, 0.0f);
                resampleAndDeliver(silence.data(), numFrames, channels_, sampleRate_);
            } else {
                // Data is float (WASAPI shared mode mix format is typically float32)
                const float* floatData = reinterpret_cast<const float*>(data);
                resampleAndDeliver(floatData, numFrames, channels_, sampleRate_);
            }

            captureClient_->ReleaseBuffer(numFrames);
            captureClient_->GetNextPacketSize(&packetLength);
        }

        Sleep(1); // ~1ms poll
    }

    if (task) AvRevertMmThreadCharacteristics(task);
}

void AudioCapture::resampleAndDeliver(const float* src, uint32_t srcFrames,
                                       uint32_t srcChannels, uint32_t srcRate) {
    // Target: 48000 Hz, 2 channels, 480-sample frames
    const uint32_t targetRate = 48000;
    const uint32_t targetChannels = 2;
    const uint32_t targetFrameSize = 480;

    if (srcRate == targetRate && srcChannels == targetChannels) {
        // No conversion needed — just accumulate and deliver in 480-sample chunks
        for (uint32_t i = 0; i < srcFrames * targetChannels; i++) {
            resampleBuf_.push_back(src[i]);
        }
    } else {
        // Resample: simple linear interpolation
        double ratio = (double)srcRate / (double)targetRate;

        for (uint32_t i = 0; ; i++) {
            double srcPos = resamplePos_ + i * ratio;
            if (srcPos >= (double)(srcFrames - 1)) {
                resamplePos_ = srcPos - (double)srcFrames;
                break;
            }

            uint32_t idx = (uint32_t)srcPos;
            float frac = (float)(srcPos - idx);

            for (uint32_t ch = 0; ch < targetChannels; ch++) {
                float s0 = 0.0f, s1 = 0.0f;
                if (ch < srcChannels) {
                    s0 = src[idx * srcChannels + ch];
                    s1 = src[(idx + 1) * srcChannels + ch];
                } else if (srcChannels == 1) {
                    // Mono to stereo: duplicate
                    s0 = src[idx * srcChannels];
                    s1 = src[(idx + 1) * srcChannels];
                }
                resampleBuf_.push_back(s0 + frac * (s1 - s0));
            }
        }
    }

    // Deliver complete 480-sample frames
    const size_t chunkSize = targetFrameSize * targetChannels;
    while (resampleBuf_.size() >= chunkSize) {
        // Copy to contiguous buffer for callback, clamping to [-1.0, 1.0]
        // WASAPI loopback can deliver values outside this range when apps output
        // hot signals or system mixing occurs. Opus expects normalized float input
        // and distorts badly on out-of-range values (causes harsh "expanded" sound).
        std::vector<float> chunk(chunkSize);
        bool clipped = false;
        for (size_t i = 0; i < chunkSize; i++) {
            float sample = resampleBuf_.front();
            resampleBuf_.pop_front();
            float absSample = sample < 0 ? -sample : sample;
            if (absSample > peakLevel_) peakLevel_ = absSample;
            if (sample > 1.0f) { sample = 1.0f; clipped = true; }
            else if (sample < -1.0f) { sample = -1.0f; clipped = true; }
            chunk[i] = sample;
        }
        peakSampleCount_ += targetFrameSize;
        if (peakSampleCount_ >= PEAK_REPORT_INTERVAL) {
            printf("\n  [Audio] Peak level: %.3f%s\n",
                   peakLevel_, peakLevel_ > 1.0f ? " (CLIPPED!)" : "");
            peakLevel_ = 0.0f;
            peakSampleCount_ = 0;
        }
        callback_(chunk.data(), targetFrameSize);
    }
}
