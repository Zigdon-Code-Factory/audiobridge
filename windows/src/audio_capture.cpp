#include "audio_capture.h"
#include <Functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>

#pragma comment(lib, "avrt.lib")

static const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
static const IID IID_IAudioClient = __uuidof(IAudioClient);
static const IID IID_IAudioClient3 = __uuidof(IAudioClient3);
static const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
static const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stop();
    cleanup();
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
}

void AudioCapture::cleanup() {
    if (tapDevice_ != INVALID_HANDLE_VALUE) {
        AudioBridgeTapEventRequest evReq{0};
        DWORD returned = 0;
        DeviceIoControl(tapDevice_, IOCTL_AUDIOBRIDGE_REGISTER_EVENT,
                        &evReq, sizeof(evReq), nullptr, 0, &returned, nullptr);
        if (tapSharedBuf_) {
            DeviceIoControl(tapDevice_, IOCTL_AUDIOBRIDGE_UNMAP_BUFFER,
                            nullptr, 0, nullptr, 0, &returned, nullptr);
            tapSharedBuf_ = nullptr;
        }
        CloseHandle(tapDevice_);
        tapDevice_ = INVALID_HANDLE_VALUE;
    }
    usingKernelTap_ = false;
    if (lowPeriodRender_) { lowPeriodRender_->Release(); lowPeriodRender_ = nullptr; }
    if (lowPeriodClient_) { lowPeriodClient_->Release(); lowPeriodClient_ = nullptr; }
    if (lowPeriodEvent_) { CloseHandle(lowPeriodEvent_); lowPeriodEvent_ = nullptr; }
    lowPeriodBufFrames_ = 0;

    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (captureEvent_) { CloseHandle(captureEvent_); captureEvent_ = nullptr; }
    deviceFriendlyName_.clear();
    resampleBuf_.clear();
    chunkBuf_.clear();
    resamplePos_ = 0.0;
    peakLevel_ = 0.0f;
    peakSampleCount_ = 0;
    sampleRate_ = 0;
    channels_ = 0;
    isFloat_ = true;
    bitsPerSample_ = 32;
}

std::vector<AudioDeviceInfo> AudioCapture::getDevices(bool loopback) {
    std::vector<AudioDeviceInfo> devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                  CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                  (void**)&enumerator);
    if (FAILED(hr)) return devices;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(loopback ? eRender : eCapture, DEVICE_STATE_ACTIVE, &collection);
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

bool AudioCapture::tryInitializeKernelTap() {
    if (!loopback_ || !device_) return false;
    if (deviceFriendlyName_.find(L"AudioBridge Virtual Speakers") == std::wstring::npos) {
        return false;
    }

    HANDLE hTap = CreateFileW(AUDIOBRIDGE_TAP_USER_PATH,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTap == INVALID_HANDLE_VALUE) {
        return false;
    }

    AudioBridgeTapMapResponse mapResp{};
    DWORD returned = 0;
    if (!DeviceIoControl(hTap, IOCTL_AUDIOBRIDGE_MAP_BUFFER,
                         nullptr, 0, &mapResp, sizeof(mapResp), &returned, nullptr) ||
        returned < sizeof(mapResp) || mapResp.userAddress == 0) {
        CloseHandle(hTap);
        return false;
    }

    auto* hdr = reinterpret_cast<AudioBridgeTapSharedBuffer*>((uintptr_t)mapResp.userAddress);
    if (hdr->magic != AUDIOBRIDGE_TAP_MAGIC || hdr->version != AUDIOBRIDGE_TAP_VERSION ||
        hdr->capacityFrames == 0 || hdr->channels == 0 || hdr->sampleRate == 0) {
        DeviceIoControl(hTap, IOCTL_AUDIOBRIDGE_UNMAP_BUFFER, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(hTap);
        return false;
    }

    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        DeviceIoControl(hTap, IOCTL_AUDIOBRIDGE_UNMAP_BUFFER, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(hTap);
        return false;
    }

    AudioBridgeTapEventRequest evReq{};
    evReq.eventHandle = (ULONG64)(uintptr_t)captureEvent_;
    if (!DeviceIoControl(hTap, IOCTL_AUDIOBRIDGE_REGISTER_EVENT,
                         &evReq, sizeof(evReq), nullptr, 0, &returned, nullptr)) {
        CloseHandle(captureEvent_);
        captureEvent_ = nullptr;
        DeviceIoControl(hTap, IOCTL_AUDIOBRIDGE_UNMAP_BUFFER, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(hTap);
        return false;
    }

    // Lock Windows audiodg.exe to the minimum WaveRT engine period (128 frames = 2.67ms)
    // via a silent companion IAudioClient3 render stream.
    if (SUCCEEDED(device_->Activate(IID_IAudioClient3, CLSCTX_ALL, nullptr, (void**)&lowPeriodClient_)) &&
        lowPeriodClient_) {
        WAVEFORMATEX* wf = nullptr;
        if (SUCCEEDED(lowPeriodClient_->GetMixFormat(&wf)) && wf) {
            UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
            if (SUCCEEDED(lowPeriodClient_->GetSharedModeEnginePeriod(wf, &defP, &fundP, &minP, &maxP)) &&
                minP > 0 && minP < defP) {
                lowPeriodEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (lowPeriodEvent_ &&
                    SUCCEEDED(lowPeriodClient_->InitializeSharedAudioStream(
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, wf, nullptr)) &&
                    SUCCEEDED(lowPeriodClient_->SetEventHandle(lowPeriodEvent_)) &&
                    SUCCEEDED(lowPeriodClient_->GetBufferSize(&lowPeriodBufFrames_)) &&
                    SUCCEEDED(lowPeriodClient_->GetService(IID_IAudioRenderClient, (void**)&lowPeriodRender_))) {
                    BYTE* pData = nullptr;
                    if (SUCCEEDED(lowPeriodRender_->GetBuffer(lowPeriodBufFrames_, &pData))) {
                        lowPeriodRender_->ReleaseBuffer(lowPeriodBufFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                    printf("[CAPTURE] Locked AudioBridge WaveRT engine period to %u frames (%.2f ms)\n",
                           minP, minP * 1000.0 / wf->nSamplesPerSec);
                }
            }
            CoTaskMemFree(wf);
        }
    }

    tapDevice_ = hTap;
    tapSharedBuf_ = hdr;
    usingKernelTap_ = true;
    sampleRate_ = hdr->sampleRate;
    channels_ = hdr->channels;
    bitsPerSample_ = 32;
    isFloat_ = true;

    printf("[CAPTURE] Attached to Direct Kernel WaveRT Tap (\\\\.\\AudioBridgeTap): %u Hz, %u ch, quantum=%u frames (%.2f ms)\n",
           sampleRate_, channels_, hdr->quantumFrames, hdr->quantumFrames * 1000.0 / sampleRate_);
    return true;
}

bool AudioCapture::initialize(const std::wstring& deviceId, bool loopback) {
    loopback_ = loopback;
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
        hr = enumerator_->GetDefaultAudioEndpoint(loopback_ ? eRender : eCapture, eConsole, &device_);
    } else {
        hr = enumerator_->GetDevice(deviceId.c_str(), &device_);
    }
    if (FAILED(hr)) {
        printf("Failed to get audio endpoint: 0x%08lx\n", hr);
        return false;
    }

    // Read endpoint friendly name for logging & Tier 2 kernel tap detection
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
            deviceFriendlyName_ = varName.pwszVal;
        }
        PropVariantClear(&varName);
        props->Release();
    }

    // Tier 2 fast path: bypass audiodg.exe's 10ms loopback APO via direct WaveRT shared memory
    if (tryInitializeKernelTap()) {
        return true;
    }

    // For recording endpoints (eCapture), try IAudioClient3 low-period shared capture first
    WAVEFORMATEX* mixFormat = nullptr;
    bool initializedViaAc3 = false;
    if (!loopback_) {
        IAudioClient3* ac3 = nullptr;
        if (SUCCEEDED(device_->Activate(IID_IAudioClient3, CLSCTX_ALL, nullptr, (void**)&ac3)) && ac3) {
            if (SUCCEEDED(ac3->GetMixFormat(&mixFormat)) && mixFormat) {
                UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
                if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(mixFormat, &defP, &fundP, &minP, &maxP)) &&
                    minP > 0 && minP < defP) {
                    if (SUCCEEDED(ac3->InitializeSharedAudioStream(
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, mixFormat, nullptr))) {
                        captureEvent_ = CreateEventW(nullptr, false, false, nullptr);
                        if (captureEvent_ && SUCCEEDED(ac3->SetEventHandle(captureEvent_))) {
                            audioClient_ = ac3;
                            ac3 = nullptr;
                            initializedViaAc3 = true;
                            printf("[CAPTURE] Initialized IAudioClient3 low-period capture: %u frames (%.2f ms)\n",
                                   minP, minP * 1000.0 / mixFormat->nSamplesPerSec);
                        } else if (captureEvent_) {
                            CloseHandle(captureEvent_);
                            captureEvent_ = nullptr;
                        }
                    }
                }
            }
            if (ac3) ac3->Release();
        }
    }

    if (!initializedViaAc3) {
        if (mixFormat) { CoTaskMemFree(mixFormat); mixFormat = nullptr; }
        hr = device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&audioClient_);
        if (FAILED(hr)) {
            printf("Failed to activate audio client: 0x%08lx\n", hr);
            return false;
        }

        hr = audioClient_->GetMixFormat(&mixFormat);
        if (FAILED(hr)) {
            printf("Failed to get mix format: 0x%08lx\n", hr);
            return false;
        }
    }

    sampleRate_ = mixFormat->nSamplesPerSec;
    channels_ = mixFormat->nChannels;
    bitsPerSample_ = mixFormat->wBitsPerSample;

    // Determine if format is float
    if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat_ = true;
    } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
        isFloat_ = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        isFloat_ = false;
    }

    fprintf(stderr, "Audio device '%ls': %u Hz, %u channels, %u bits, %s\n",
           deviceFriendlyName_.c_str(), sampleRate_, channels_, bitsPerSample_, isFloat_ ? "float" : "int");
    fflush(stderr);

    if (!initializedViaAc3) {
        // Prefer event-driven WASAPI signaling for both recording AND loopback (supported
        // since Windows 10 1703) so the capture thread wakes immediately on engine periods
        // without Sleep(1) polling jitter. Automatically fall back to polling if a driver
        // rejects event-driven loopback.
        DWORD eventFlags = (loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0) | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       eventFlags,
                                       0, 0, mixFormat, nullptr);
        if (SUCCEEDED(hr)) {
            captureEvent_ = CreateEventW(nullptr, false, false, nullptr);
            if (!captureEvent_) {
                printf("Failed to create capture event\n");
                CoTaskMemFree(mixFormat);
                return false;
            }
            hr = audioClient_->SetEventHandle(captureEvent_);
            if (FAILED(hr)) {
                printf("[CAPTURE] SetEventHandle failed (0x%08lx)\n", hr);
                CloseHandle(captureEvent_);
                captureEvent_ = nullptr;
            }
        }

        if (FAILED(hr) && loopback_) {
            printf("[CAPTURE] Event-driven loopback unsupported (0x%08lx), falling back to polling\n", hr);
            audioClient_->Release();
            audioClient_ = nullptr;
            hr = device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&audioClient_);
            if (FAILED(hr)) {
                printf("Failed to re-activate audio client: 0x%08lx\n", hr);
                CoTaskMemFree(mixFormat);
                return false;
            }
            hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_LOOPBACK,
                                           100000, 0, mixFormat, nullptr);
        }

        if (FAILED(hr)) {
            printf("Failed to initialize audio client: 0x%08lx\n", hr);
            CoTaskMemFree(mixFormat);
            return false;
        }
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

    if (usingKernelTap_) {
        if (lowPeriodClient_ && lowPeriodRender_) {
            lowPeriodClient_->Start();
        }
        running_ = true;
        thread_ = std::thread(&AudioCapture::tapCaptureThread, this);
        return true;
    }

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
    if (captureEvent_) SetEvent(captureEvent_);
    if (lowPeriodEvent_) SetEvent(lowPeriodEvent_);
    if (thread_.joinable()) thread_.join();
    if (lowPeriodClient_) lowPeriodClient_->Stop();
    if (audioClient_) audioClient_->Stop();
}

void AudioCapture::tapCaptureThread() {
    printf("[CAPTURE] Direct Kernel WaveRT Tap thread started (tid=%lu, rate=%u, ch=%u)\n",
           GetCurrentThreadId(), sampleRate_, channels_);

    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const float* ringSamples = AudioBridgeTapGetSamplesConst(tapSharedBuf_);
    const uint32_t capFrames = tapSharedBuf_->capacityFrames;
    LONG64 readCursor = InterlockedCompareExchange64(&tapSharedBuf_->writeFrameCursor, 0, 0);
    std::vector<float> tempBuf(1024 * channels_);

    while (running_) {
        HANDLE handles[2];
        DWORD handleCount = 0;
        if (captureEvent_) handles[handleCount++] = captureEvent_;
        if (lowPeriodEvent_) handles[handleCount++] = lowPeriodEvent_;

        DWORD waitRes = (handleCount > 0)
            ? WaitForMultipleObjects(handleCount, handles, FALSE, 10)
            : WAIT_TIMEOUT;

        if (!running_) break;

        // Keep silent companion IAudioClient3 pacemaker fed so audiodg.exe stays at 128 frames
        if (lowPeriodClient_ && lowPeriodRender_ && lowPeriodBufFrames_ > 0 &&
            (waitRes == WAIT_OBJECT_0 + 1 || waitRes == WAIT_TIMEOUT)) {
            UINT32 pad = 0;
            if (SUCCEEDED(lowPeriodClient_->GetCurrentPadding(&pad)) && lowPeriodBufFrames_ > pad) {
                BYTE* pData = nullptr;
                UINT32 toWrite = lowPeriodBufFrames_ - pad;
                if (SUCCEEDED(lowPeriodRender_->GetBuffer(toWrite, &pData))) {
                    lowPeriodRender_->ReleaseBuffer(toWrite, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
        }

        LONG64 writeCursor = InterlockedCompareExchange64(&tapSharedBuf_->writeFrameCursor, 0, 0);
        if (writeCursor < readCursor) {
            // Driver stream restarted and reset its cursor; resync instead of stalling forever
            readCursor = writeCursor;
        }
        if (writeCursor > readCursor) {
            LONG64 avail = writeCursor - readCursor;
            if (avail > (LONG64)capFrames) {
                // Reader fell behind cyclic ring capacity; jump to latest quantum
                const uint32_t q = tapSharedBuf_->quantumFrames;
                readCursor = writeCursor - ((q > 0 && q <= capFrames) ? q : AUDIOBRIDGE_TAP_DEFAULT_QUANTUM);
                avail = writeCursor - readCursor;
            }
            while (avail > 0 && running_) {
                uint32_t chunkFrames = (avail > 512) ? 512u : (uint32_t)avail;
                if (tempBuf.size() < chunkFrames * channels_) {
                    tempBuf.resize(chunkFrames * channels_);
                }
                for (uint32_t f = 0; f < chunkFrames; ++f) {
                    uint32_t ringIdx = (uint32_t)((readCursor + f) % capFrames) * channels_;
                    for (uint32_t c = 0; c < channels_; ++c) {
                        tempBuf[f * channels_ + c] = ringSamples[ringIdx + c];
                    }
                }
                readCursor += chunkFrames;
                avail -= chunkFrames;
                resampleAndDeliver(tempBuf.data(), chunkFrames, channels_, sampleRate_);
            }
        } else if (tapSharedBuf_->activeStreams == 0 && waitRes == WAIT_TIMEOUT) {
            // Deliver idle silence quantum when no audio stream is active on the endpoint
            uint32_t qFrames = tapSharedBuf_->quantumFrames ? tapSharedBuf_->quantumFrames : 128u;
            std::vector<float> silence(qFrames * channels_, 0.0f);
            resampleAndDeliver(silence.data(), qFrames, channels_, sampleRate_);
        }
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    printf("[CAPTURE] Direct Kernel WaveRT Tap thread exiting\n");
}

void AudioCapture::captureThread() {
    printf("[CAPTURE] Thread started (tid=%lu, rate=%u, ch=%u, %s, wait=%s, dev='%ls')\n",
           GetCurrentThreadId(), sampleRate_, channels_, isFloat_ ? "float" : "int",
           captureEvent_ ? "event" : "poll", deviceFriendlyName_.c_str());

    // Boost thread priority for low latency
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!task) {
        printf("[CAPTURE] WARN: AvSetMmThreadCharacteristics failed (err=%lu)\n", GetLastError());
    }

    // Diagnostic counters
    uint64_t totalFrames = 0;
    uint64_t silenceFrames = 0;
    uint64_t deliveryCount = 0;
    uint64_t getBufferFailCount = 0;
    uint64_t emptyPollCount = 0;        // times we polled and got nothing
    auto lastStatsTime = std::chrono::steady_clock::now();
    auto lastDeliveryTime = std::chrono::steady_clock::now();
    double maxGapMs = 0.0;
    double totalGapMs = 0.0;
    uint64_t gapCount = 0;
    uint64_t largeGapCount = 0;  // gaps > 15ms (should be ~10ms)
    size_t maxResampleBuf = 0;
    size_t minResampleBuf = 999999;
    uint64_t discontinuityCount = 0; // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY

    while (running_) {
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                printf("[CAPTURE] Audio device invalidated (AUDCLNT_E_DEVICE_INVALIDATED)\n");
                if (onDeviceInvalidated_) onDeviceInvalidated_();
            } else {
                printf("[CAPTURE] GetNextPacketSize failed: 0x%08lx\n", hr);
            }
            break;
        }

        if (packetLength == 0) {
            emptyPollCount++;
        }

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                getBufferFailCount++;
                if (getBufferFailCount <= 5 || (getBufferFailCount % 100) == 0) {
                    printf("[CAPTURE] GetBuffer failed #%llu: 0x%08lx\n", getBufferFailCount, hr);
                }
                break;
            }

            // Track data discontinuity flags (indicates WASAPI buffer overrun)
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                discontinuityCount++;
                printf("[CAPTURE] *** DATA DISCONTINUITY #%llu (flags=0x%lx, frames=%u) — WASAPI buffer overrun, expect pop!\n",
                       discontinuityCount, flags, numFrames);
            }

            // Track inter-delivery timing
            auto now = std::chrono::steady_clock::now();
            double gapMs = std::chrono::duration<double, std::milli>(now - lastDeliveryTime).count();
            lastDeliveryTime = now;
            totalGapMs += gapMs;
            gapCount++;
            if (gapMs > maxGapMs) maxGapMs = gapMs;
            if (gapMs > 15.0) {
                largeGapCount++;
                if (largeGapCount <= 20 || (largeGapCount % 50) == 0) {
                    printf("[CAPTURE] LARGE GAP #%llu: %.1fms between deliveries (frames=%u, resBuf=%zu)\n",
                           largeGapCount, gapMs, numFrames, resampleBuf_.size());
                }
            }

            totalFrames += numFrames;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silenceFrames += numFrames;
                std::vector<float> silence(numFrames * channels_, 0.0f);
                resampleAndDeliver(silence.data(), numFrames, channels_, sampleRate_);
                deliveryCount++;
            } else if (isFloat_) {
                const float* floatData = reinterpret_cast<const float*>(data);
                resampleAndDeliver(floatData, numFrames, channels_, sampleRate_);
                deliveryCount++;
            } else {
                const int16_t* intData = reinterpret_cast<const int16_t*>(data);
                uint32_t totalSamples = numFrames * channels_;
                std::vector<float> floatBuf(totalSamples);
                for (uint32_t i = 0; i < totalSamples; i++) {
                    floatBuf[i] = intData[i] / 32768.0f;
                }
                resampleAndDeliver(floatBuf.data(), numFrames, channels_, sampleRate_);
                deliveryCount++;
            }

            size_t resBufAfter = resampleBuf_.size();
            if (resBufAfter > maxResampleBuf) maxResampleBuf = resBufAfter;
            if (resBufAfter < minResampleBuf) minResampleBuf = resBufAfter;

            captureClient_->ReleaseBuffer(numFrames);
            captureClient_->GetNextPacketSize(&packetLength);
        }

        // Periodic stats logging (every 5 seconds)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastStatsTime).count();
        if (elapsed >= 5) {
            double avgGapMs = gapCount > 0 ? totalGapMs / gapCount : 0.0;
            printf("[CAPTURE] Stats: frames=%llu, silence=%.1f%%, deliveries=%llu, getBufFails=%llu\n"
                   "         resBuf=%zu (min=%zu, max=%zu), emptyPolls=%llu\n"
                   "         gaps: avg=%.1fms, max=%.1fms, large(>15ms)=%llu, discontinuities=%llu\n",
                   totalFrames,
                   totalFrames > 0 ? (100.0 * silenceFrames / totalFrames) : 0.0,
                   deliveryCount, getBufferFailCount,
                   resampleBuf_.size(), minResampleBuf, maxResampleBuf, emptyPollCount,
                   avgGapMs, maxGapMs, largeGapCount, discontinuityCount);
            // Reset per-period counters
            maxGapMs = 0.0;
            totalGapMs = 0.0;
            gapCount = 0;
            largeGapCount = 0;
            emptyPollCount = 0;
            maxResampleBuf = 0;
            minResampleBuf = 999999;
            lastStatsTime = now;
        }

        if (!running_) break;

        // Wait for next buffer: event-driven (wakes immediately on WASAPI quantum) or 1ms poll fallback
        if (captureEvent_) {
            WaitForSingleObject(captureEvent_, 25);
        } else {
            Sleep(1);
        }
    }

    printf("[CAPTURE] Thread exiting (total frames=%llu, discontinuities=%llu)\n",
           totalFrames, discontinuityCount);
    if (task) AvRevertMmThreadCharacteristics(task);
}

void AudioCapture::resampleAndDeliver(const float* src, uint32_t srcFrames,
                                       uint32_t srcChannels, uint32_t srcRate) {
    // Target: 48000 Hz, 2 channels, configurable frame size
    const uint32_t targetRate = 48000;
    const uint32_t targetChannels = 2;
    const uint32_t targetFrameSize = targetFrameSize_.load(std::memory_order_relaxed);

    if (srcFrames == 0 || srcChannels == 0) return;

    if (srcRate == targetRate && srcChannels == targetChannels) {
        // No conversion needed — accumulate and deliver in targetFrameSize chunks
        for (uint32_t i = 0; i < srcFrames * targetChannels; i++) {
            resampleBuf_.push_back(src[i]);
        }
    } else if (srcRate == targetRate) {
        // Same rate, different channels — just upmix/downmix without resampling
        for (uint32_t i = 0; i < srcFrames; i++) {
            float sample = src[i * srcChannels]; // take first channel
            for (uint32_t ch = 0; ch < targetChannels; ch++) {
                if (ch < srcChannels) {
                    resampleBuf_.push_back(src[i * srcChannels + ch]);
                } else {
                    resampleBuf_.push_back(sample); // duplicate mono to stereo
                }
            }
        }
    } else {
        // Resample: simple linear interpolation
        double ratio = (double)srcRate / (double)targetRate;

        if (srcFrames < 2) {
            // Too few frames to interpolate
            resamplePos_ = 0.0;
            return;
        }

        for (uint32_t i = 0; ; i++) {
            double srcPos = resamplePos_ + i * ratio;
            if (srcPos >= (double)(srcFrames - 1)) {
                resamplePos_ = srcPos - (double)srcFrames;
                break;
            }

            uint32_t idx = (uint32_t)srcPos;
            float frac = (float)(srcPos - idx);
            uint32_t idx1 = idx + 1 < srcFrames ? idx + 1 : idx;

            for (uint32_t ch = 0; ch < targetChannels; ch++) {
                float s0 = 0.0f, s1 = 0.0f;
                if (ch < srcChannels) {
                    s0 = src[idx * srcChannels + ch];
                    s1 = src[idx1 * srcChannels + ch];
                } else if (srcChannels == 1) {
                    // Mono to stereo: duplicate
                    s0 = src[idx * srcChannels];
                    s1 = src[idx1 * srcChannels];
                }
                resampleBuf_.push_back(s0 + frac * (s1 - s0));
            }
        }
    }

    // Deliver complete frames reusing chunkBuf_
    const size_t chunkSize = targetFrameSize * targetChannels;
    if (chunkBuf_.size() != chunkSize) {
        chunkBuf_.resize(chunkSize);
    }
    while (resampleBuf_.size() >= chunkSize) {
        // Copy to contiguous buffer for callback, clamping to [-1.0, 1.0]
        // WASAPI loopback can deliver values outside this range when apps output
        // hot signals or system mixing occurs. Opus expects normalized float input
        // and distorts badly on out-of-range values (causes harsh "expanded" sound).
        for (size_t i = 0; i < chunkSize; i++) {
            float sample = resampleBuf_.front();
            resampleBuf_.pop_front();
            float absSample = sample < 0 ? -sample : sample;
            if (absSample > peakLevel_) peakLevel_ = absSample;
            if (sample > 1.0f) { sample = 1.0f; }
            else if (sample < -1.0f) { sample = -1.0f; }
            chunkBuf_[i] = sample;
        }
        peakSampleCount_ += targetFrameSize;
        if (peakSampleCount_ >= PEAK_REPORT_INTERVAL) {
            printf("\n  [Audio] Peak level: %.3f%s\n",
                   peakLevel_, peakLevel_ > 1.0f ? " (CLIPPED!)" : "");
            peakLevel_ = 0.0f;
            peakSampleCount_ = 0;
        }
        callback_(chunkBuf_.data(), targetFrameSize);
    }
}
