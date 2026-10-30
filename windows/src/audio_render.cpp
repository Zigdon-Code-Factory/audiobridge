#include "audio_render.h"
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "avrt.lib")

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

    // Prefer event-driven shared WASAPI render at engine default period (~10ms)
    hr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0,
        0,
        pwfx,
        nullptr);

    if (SUCCEEDED(hr)) {
        renderEvent_ = CreateEventW(nullptr, false, false, nullptr);
        if (renderEvent_) {
            hr = audioClient_->SetEventHandle(renderEvent_);
            if (FAILED(hr)) {
                CloseHandle(renderEvent_);
                renderEvent_ = nullptr;
            }
        } else {
            hr = E_FAIL;
        }
    }

    if (FAILED(hr)) {
        // Fallback to 12ms polled buffer if driver rejects event-driven render
        audioClient_->Release();
        audioClient_ = nullptr;
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
        if (FAILED(hr)) {
            CoTaskMemFree(pwfx);
            return false;
        }
        hr = audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            120000, // 12ms buffer
            0,
            pwfx,
            nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(pwfx);
            return false;
        }
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
    if (renderEvent_) { CloseHandle(renderEvent_); renderEvent_ = nullptr; }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

bool AudioRender::start() {
    if (!audioClient_ || !renderClient_) return false;

    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        audioBuffer_.clear();
        primed_ = false;
        windowMinFrames_ = (std::numeric_limits<size_t>::max)();
        windowPushCount_ = 0;
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) return false;

    running_ = true;
    thread_ = std::thread(&AudioRender::renderThread, this);
    return true;
}

void AudioRender::stop() {
    running_ = false;
    if (renderEvent_) SetEvent(renderEvent_);
    if (thread_.joinable()) thread_.join();
    if (audioClient_) audioClient_->Stop();
    std::lock_guard<std::mutex> lock(bufferMutex_);
    audioBuffer_.clear();
    primed_ = false;
    windowMinFrames_ = (std::numeric_limits<size_t>::max)();
    windowPushCount_ = 0;
}

void AudioRender::pushAudio(const float* data, uint32_t frameCount) {
    if (!running_ || deviceChannels_ == 0 || deviceSampleRate_ == 0 || frameCount == 0) return;

    // Convert from 48kHz mono float -> device sample rate, device channels, float
    // Step 1: Upmix mono to device channel count
    std::vector<float> channelMixed(frameCount * deviceChannels_);
    for (uint32_t i = 0; i < frameCount; i++) {
        float sample = data[i];
        for (uint32_t ch = 0; ch < deviceChannels_; ch++) {
            channelMixed[i * deviceChannels_ + ch] = sample;
        }
    }

    // Step 2: Resample if device sample rate differs from 48kHz
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
    if (primed_) {
        size_t curFramesBefore = audioBuffer_.size() / deviceChannels_;
        if (curFramesBefore < windowMinFrames_) {
            windowMinFrames_ = curFramesBefore;
        }
    }

    audioBuffer_.insert(audioBuffer_.end(), outputPtr->begin(), outputPtr->end());

    // Hard cap on buffer size to bound worst-case latency spikes (max 45ms at device rate)
    const size_t maxFrames = (deviceSampleRate_ * 45) / 1000;
    const size_t maxSamples = maxFrames * deviceChannels_;
    if (audioBuffer_.size() > maxSamples) {
        audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.begin() + (audioBuffer_.size() - maxSamples));
    }

    // Windowed (~350ms) minimum-depth catch-up to drain accumulated drift back to ~12ms
    if (primed_) {
        windowPushCount_++;
        const uint32_t pushMs = (std::max)(5u, (frameCount * 1000u) / INPUT_SAMPLE_RATE);
        const uint32_t windowPushes = (std::max)(20u, 350u / pushMs);
        if (windowPushCount_ >= windowPushes) {
            const size_t targetFrames = (deviceSampleRate_ * 12) / 1000;
            const size_t curFrames = audioBuffer_.size() / deviceChannels_;
            if (windowMinFrames_ > targetFrames && curFrames > targetFrames) {
                size_t dropFrames = (std::min)(windowMinFrames_ - targetFrames, curFrames - targetFrames);
                audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.begin() + dropFrames * deviceChannels_);
            }
            windowMinFrames_ = (std::numeric_limits<size_t>::max)();
            windowPushCount_ = 0;
        }
    }
}

void AudioRender::renderThread() {
    DWORD taskIndex = 0;
    HANDLE mmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    UINT32 bufferFrameCount = 0;
    audioClient_->GetBufferSize(&bufferFrameCount);

    REFERENCE_TIME defaultPeriod = 100000; // 10ms fallback
    REFERENCE_TIME minPeriod = 0;
    audioClient_->GetDevicePeriod(&defaultPeriod, &minPeriod);
    UINT32 quantumFrames = static_cast<UINT32>((static_cast<uint64_t>(deviceSampleRate_) * defaultPeriod) / 10000000ULL);
    if (quantumFrames == 0 || quantumFrames > bufferFrameCount) {
        quantumFrames = bufferFrameCount;
    }

    const uint32_t bytesPerFrame = deviceChannels_ * (deviceBitsPerSample_ / 8);

    while (running_) {
        UINT32 numFramesPadding = 0;
        HRESULT hr = audioClient_->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                if (onDeviceInvalidated_) onDeviceInvalidated_();
            }
            break;
        }

        UINT32 numFramesAvailable = bufferFrameCount > numFramesPadding ? (bufferFrameCount - numFramesPadding) : 0;
        uint32_t framesWritten = 0;

        if (numFramesAvailable > 0) {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            uint32_t availableFrames = static_cast<uint32_t>(audioBuffer_.size()) / deviceChannels_;
            const uint32_t primeTarget = (std::max)(quantumFrames, (deviceSampleRate_ * 12) / 1000);

            if (!primed_ && availableFrames >= primeTarget) {
                primed_ = true;
            }

            if (primed_ && availableFrames > 0) {
                framesWritten = (std::min)(numFramesAvailable, availableFrames);
                BYTE* pData = nullptr;
                hr = renderClient_->GetBuffer(framesWritten, &pData);
                if (FAILED(hr)) {
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        if (onDeviceInvalidated_) onDeviceInvalidated_();
                    }
                    break;
                }

                if (deviceIsFloat_) {
                    memcpy(pData, audioBuffer_.data(), framesWritten * deviceChannels_ * sizeof(float));
                } else {
                    auto* out16 = reinterpret_cast<int16_t*>(pData);
                    for (uint32_t i = 0; i < framesWritten * deviceChannels_; i++) {
                        float s = audioBuffer_[i];
                        if (s > 1.0f) s = 1.0f;
                        if (s < -1.0f) s = -1.0f;
                        out16[i] = static_cast<int16_t>(s * 32767.0f);
                    }
                }

                renderClient_->ReleaseBuffer(framesWritten, 0);
                audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.begin() + framesWritten * deviceChannels_);

                size_t remFrames = audioBuffer_.size() / deviceChannels_;
                if (remFrames < windowMinFrames_) {
                    windowMinFrames_ = remFrames;
                }
            } else if (primed_ && availableFrames == 0 && numFramesPadding == 0) {
                // Mic stream ended or underran completely — re-prime cleanly on next speech burst
                primed_ = false;
                windowMinFrames_ = (std::numeric_limits<size_t>::max)();
                windowPushCount_ = 0;
            }
        }

        // Only pad up to a single engine quantum with silent frames when idle/unprimed
        // (avoids stuffing the full WASAPI buffer with 20ms of trailing silence ahead of new packets)
        UINT32 totalQueued = numFramesPadding + framesWritten;
        if (totalQueued < quantumFrames && totalQueued < bufferFrameCount) {
            UINT32 silenceFrames = (std::min)(quantumFrames - totalQueued, bufferFrameCount - totalQueued);
            BYTE* pData = nullptr;
            hr = renderClient_->GetBuffer(silenceFrames, &pData);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    if (onDeviceInvalidated_) onDeviceInvalidated_();
                }
                break;
            }
            memset(pData, 0, silenceFrames * bytesPerFrame);
            renderClient_->ReleaseBuffer(silenceFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        if (!running_) break;

        if (renderEvent_) {
            WaitForSingleObject(renderEvent_, 10);
        } else {
            Sleep(2);
        }
    }

    if (mmcssTask) AvRevertMmThreadCharacteristics(mmcssTask);
}
