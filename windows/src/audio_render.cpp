#include "audio_render.h"
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>

AudioRender::AudioRender() {}

AudioRender::~AudioRender() {
    cleanup();
}

std::vector<std::pair<std::wstring, std::wstring>> AudioRender::getDevices() {
    std::vector<std::pair<std::wstring, std::wstring>> devices;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    if (SUCCEEDED(hr)) {
        IMMDeviceCollection* pDevices = nullptr;
        hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices);

        if (SUCCEEDED(hr)) {
            UINT count = 0;
            pDevices->GetCount(&count);

            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pDevices->Item(i, &pDev))) {
                    LPWSTR pWsId = nullptr;
                    if (SUCCEEDED(pDev->GetId(&pWsId))) {
                        IPropertyStore* pProps = nullptr;
                        std::wstring name = L"Unknown Device";

                        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps))) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                                name = varName.pwszVal;
                                PropVariantClear(&varName);
                            }
                            pProps->Release();
                        }

                        devices.push_back({pWsId, name});
                        CoTaskMemFree(pWsId);
                    }
                    pDev->Release();
                }
            }
            pDevices->Release();
        }
        pEnum->Release();
    }

    if (coInit) CoUninitialize();
    return devices;
}

bool AudioRender::initialize(const std::wstring& deviceId) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized_ = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr)) return false;

    if (deviceId.empty()) {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    } else {
        hr = enumerator_->GetDevice(deviceId.c_str(), &device_);
    }

    if (FAILED(hr) || !device_) return false;

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
    if (FAILED(hr)) return false;

    WAVEFORMATEX* pwfx = nullptr;
    hr = audioClient_->GetMixFormat(&pwfx);
    if (FAILED(hr)) return false;

    // Store device format properties for conversion
    deviceSampleRate_ = pwfx->nSamplesPerSec;
    deviceChannels_ = pwfx->nChannels;
    deviceBitsPerSample_ = pwfx->wBitsPerSample;

    // Determine if format is float
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        deviceIsFloat_ = true;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
        deviceIsFloat_ = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        deviceIsFloat_ = false;
    }

    REFERENCE_TIME hnsRequestedDuration = 200000; // 20ms buffer
    hr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        hnsRequestedDuration,
        0,
        pwfx,
        nullptr);

    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        return false;
    }

    hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        return false;
    }

    CoTaskMemFree(pwfx);
    return true;
}

void AudioRender::cleanup() {
    stop();
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

bool AudioRender::start() {
    if (!audioClient_ || !renderClient_) return false;

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) return false;

    running_ = true;
    thread_ = std::thread(&AudioRender::renderThread, this);
    return true;
}

void AudioRender::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (audioClient_) audioClient_->Stop();
    std::lock_guard<std::mutex> lock(bufferMutex_);
    audioBuffer_.clear();
}

void AudioRender::pushAudio(const float* data, uint32_t frameCount) {
    if (!running_ || deviceChannels_ == 0 || deviceSampleRate_ == 0) return;

    // Convert from 48kHz mono float → device sample rate, device channels, float
    // Step 1: Upmix mono to device channel count
    std::vector<float> channelMixed(frameCount * deviceChannels_);
    for (uint32_t i = 0; i < frameCount; i++) {
        float sample = data[i];
        for (uint32_t ch = 0; ch < deviceChannels_; ch++) {
            channelMixed[i * deviceChannels_ + ch] = sample;
        }
    }

    // Step 2: Resample if device sample rate differs from 48kHz
    // Use simple linear interpolation resampler
    std::vector<float>* outputPtr = &channelMixed;
    std::vector<float> resampled;

    if (deviceSampleRate_ != INPUT_SAMPLE_RATE) {
        double ratio = (double)deviceSampleRate_ / (double)INPUT_SAMPLE_RATE;
        uint32_t outFrames = (uint32_t)(frameCount * ratio + 0.5);
        resampled.resize(outFrames * deviceChannels_);

        for (uint32_t i = 0; i < outFrames; i++) {
            double srcPos = (double)i / ratio;
            uint32_t srcIdx = (uint32_t)srcPos;
            double frac = srcPos - srcIdx;

            uint32_t idx0 = srcIdx < frameCount ? srcIdx : frameCount - 1;
            uint32_t idx1 = (srcIdx + 1) < frameCount ? srcIdx + 1 : frameCount - 1;

            for (uint32_t ch = 0; ch < deviceChannels_; ch++) {
                float s0 = channelMixed[idx0 * deviceChannels_ + ch];
                float s1 = channelMixed[idx1 * deviceChannels_ + ch];
                resampled[i * deviceChannels_ + ch] = (float)(s0 + (s1 - s0) * frac);
            }
        }
        outputPtr = &resampled;
    }

    std::lock_guard<std::mutex> lock(bufferMutex_);
    audioBuffer_.insert(audioBuffer_.end(), outputPtr->begin(), outputPtr->end());

    // Hard limit on buffer size to avoid huge latency (max 100ms at device rate)
    size_t maxFrames = deviceSampleRate_ / 10;
    size_t maxSamples = maxFrames * deviceChannels_;
    if (audioBuffer_.size() > maxSamples) {
        audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.begin() + (audioBuffer_.size() - maxSamples));
    }
}

void AudioRender::renderThread() {
    UINT32 bufferFrameCount;
    audioClient_->GetBufferSize(&bufferFrameCount);

    uint32_t bytesPerFrame = deviceChannels_ * (deviceBitsPerSample_ / 8);

    while (running_) {
        UINT32 numFramesPadding;
        HRESULT hr = audioClient_->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) {
            // Check for device invalidation
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                if (onDeviceInvalidated_) onDeviceInvalidated_();
            }
            break;
        }

        UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;

        if (numFramesAvailable > 0) {
            BYTE* pData;
            hr = renderClient_->GetBuffer(numFramesAvailable, &pData);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    if (onDeviceInvalidated_) onDeviceInvalidated_();
                }
                break;
            }

            uint32_t framesToWrite = 0;
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                uint32_t availableFrames = static_cast<uint32_t>(audioBuffer_.size()) / deviceChannels_;
                framesToWrite = std::min(numFramesAvailable, availableFrames);

                if (framesToWrite > 0) {
                    if (deviceIsFloat_) {
                        // Device expects float — direct copy
                        memcpy(pData, audioBuffer_.data(), framesToWrite * deviceChannels_ * sizeof(float));
                    } else {
                        // Device expects int16 — convert
                        auto* out16 = reinterpret_cast<int16_t*>(pData);
                        for (uint32_t i = 0; i < framesToWrite * deviceChannels_; i++) {
                            float s = audioBuffer_[i];
                            if (s > 1.0f) s = 1.0f;
                            if (s < -1.0f) s = -1.0f;
                            out16[i] = static_cast<int16_t>(s * 32767.0f);
                        }
                    }
                    audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.begin() + framesToWrite * deviceChannels_);
                }
            }

            // Fill remainder with silence
            if (framesToWrite < numFramesAvailable) {
                size_t offset = framesToWrite * bytesPerFrame;
                size_t silenceBytes = (numFramesAvailable - framesToWrite) * bytesPerFrame;
                memset(pData + offset, 0, silenceBytes);
            }

            renderClient_->ReleaseBuffer(numFramesAvailable, 0);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
