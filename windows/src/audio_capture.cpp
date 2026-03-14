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
    if (captureClient_) captureClient_->Release();
    if (audioClient_) audioClient_->Release();
    if (device_) device_->Release();
    if (enumerator_) enumerator_->Release();
}

bool AudioCapture::initialize() {
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                  CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                  (void**)&enumerator_);
    if (FAILED(hr)) {
        printf("Failed to create device enumerator: 0x%08lx\n", hr);
        return false;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        printf("Failed to get default audio endpoint: 0x%08lx\n", hr);
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
        if (FAILED(hr)) break;

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
        // Copy to contiguous buffer for callback
        std::vector<float> chunk(chunkSize);
        for (size_t i = 0; i < chunkSize; i++) {
            chunk[i] = resampleBuf_.front();
            resampleBuf_.pop_front();
        }
        callback_(chunk.data(), targetFrameSize);
    }
}
