#include <Windows.h>
#include <DbgHelp.h>
#include <timeapi.h>
#pragma comment(lib, "Winmm.lib")
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <fstream>

#include "audio_capture.h"
#include "audio_render.h"
#include "virtual_microphone.h"
#include "opus_encoder.h"
#include "opus_decoder.h"
#include "network.h"
#include "gui.h"

#include <shellapi.h>
#pragma comment(lib, "dbghelp.lib")

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

using namespace winrt::Windows::Media::Control;

static std::string getExeDir() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        path = path.substr(0, lastSlash + 1);
    return path;
}

static std::string getCrashLogPath() {
    return getExeDir() + "audiobridge_crash.log";
}

static FILE* g_logFile = nullptr;

static void logPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (g_logFile) {
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fflush(g_logFile);
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ex) {
    std::ofstream log(getCrashLogPath(), std::ios::app);
    if (log.is_open()) {
        time_t now = std::time(nullptr);
        char timeBuf[64];
        ctime_s(timeBuf, sizeof(timeBuf), &now);
        log << "=== CRASH at " << timeBuf;
        log << "Exception code: 0x" << std::hex << ex->ExceptionRecord->ExceptionCode << std::dec << "\n";
        log << "Address: 0x" << std::hex << (uintptr_t)ex->ExceptionRecord->ExceptionAddress << std::dec << "\n";

        auto* ctx = ex->ContextRecord;
#ifdef _WIN64
        log << "RIP=0x" << std::hex << ctx->Rip << " RSP=0x" << ctx->Rsp << " RBP=0x" << ctx->Rbp << std::dec << "\n";
#else
        log << "EIP=0x" << std::hex << ctx->Eip << " ESP=0x" << ctx->Esp << " EBP=0x" << ctx->Ebp << std::dec << "\n";
#endif
        log << "===\n\n";
        log.flush();
    }

    fprintf(stderr, "\n!!! AudioBridge crashed (code 0x%08lX). See audiobridge_crash.log\n",
            ex->ExceptionRecord->ExceptionCode);

    return EXCEPTION_EXECUTE_HANDLER;
}

// --- Settings persistence ---

struct AppSettings {
    int jitterBufferMs = 10;
    int frameSizeMs = 2;           // 2 => 2.5ms (120 samples), 5, 10, or 20 ms
    bool rawPcm = true;            // true = zero-lookahead 16-bit PCM, false = Opus
    bool loopbackMode = true;
    bool preferNativeMic = true;
    std::string inputDeviceName;   // empty = default
    std::string outputDeviceName;  // empty = default
};

static uint32_t samplesForFrameSetting(int frameSettingMs) {
    if (frameSettingMs == 2) return 120; // 2.5 ms @ 48 kHz
    if (frameSettingMs == 5) return 240;
    if (frameSettingMs == 10) return 480;
    if (frameSettingMs == 20) return 960;
    return 120;
}

static std::string labelForFrameSetting(int frameSettingMs) {
    return (frameSettingMs == 2) ? "2.5" : std::to_string(frameSettingMs);
}

static std::string getSettingsPath() {
    return getExeDir() + "audiobridge_settings.txt";
}

static AppSettings loadSettings() {
    AppSettings s;
    std::ifstream file(getSettingsPath());
    if (!file.is_open()) return s;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "jitter") { try { s.jitterBufferMs = std::stoi(val); } catch (...) {} }
        else if (key == "frame") { try { int v = std::stoi(val); if (v==2||v==5||v==10||v==20) s.frameSizeMs = v; } catch (...) {} }
        else if (key == "codec") { s.rawPcm = (val != "opus"); }
        else if (key == "mode") { s.loopbackMode = (val != "recording"); }
        else if (key == "input") { s.inputDeviceName = val; }
        else if (key == "output") { s.outputDeviceName = val; }
        else if (key == "native_mic") { s.preferNativeMic = (val != "0"); }
    }
    return s;
}

static void saveSettings(const AppSettings& s) {
    std::ofstream file(getSettingsPath(), std::ios::trunc);
    if (!file.is_open()) return;
    file << "# AudioBridge settings\n";
    file << "jitter=" << s.jitterBufferMs << "\n";
    file << "frame=" << s.frameSizeMs << "\n";
    file << "codec=" << (s.rawPcm ? "pcm" : "opus") << "\n";
    file << "mode=" << (s.loopbackMode ? "loopback" : "recording") << "\n";
    file << "input=" << s.inputDeviceName << "\n";
    file << "output=" << s.outputDeviceName << "\n";
    file << "native_mic=" << (s.preferNativeMic ? "1" : "0") << "\n";
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetUnhandledExceptionFilter(CrashHandler);

    // Set 1ms Windows timer resolution so Sleep(1) in the capture thread actually
    // sleeps ~1ms instead of the default ~15.6ms — without this, WASAPI's 10ms
    // capture buffer overflows every poll cycle causing DATA_DISCONTINUITY gaps.
    timeBeginPeriod(1);

    // Open log file
    std::string logPath = getExeDir() + "audiobridge.log";
    g_logFile = fopen(logPath.c_str(), "a");
    if (g_logFile) {
        time_t now = std::time(nullptr);
        char timeBuf[64];
        ctime_s(timeBuf, sizeof(timeBuf), &now);
        fprintf(g_logFile, "\n=== AudioBridge started at %s", timeBuf);
        fflush(g_logFile);
        // Redirect stdout/stderr to log file so printf() from network.cpp is captured
        freopen(logPath.c_str(), "a", stdout);
        freopen(logPath.c_str(), "a", stderr);
        setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    logPrintf("[DEBUG] AudioBridge starting\n");

    // Initialize COM as STA (required by WebView2)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxA(nullptr, "COM init failed", "AudioBridge Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Get computer name for server identity
    char computerName[256] = "AudioBridge";
    DWORD nameLen = sizeof(computerName);
    GetComputerNameA(computerName, &nameLen);

    // Initialize GUI
    ServerGui gui;
    if (!gui.initialize(computerName)) {
        MessageBoxA(nullptr, "Failed to create GUI window.\nEnsure WebView2 Runtime is installed.",
                    "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    gui.addLogMessage("AudioBridge Server v1.0 starting...");

    // Load saved settings
    AppSettings settings = loadSettings();
    logPrintf("[SETTINGS] Loaded: jitter=%d, frame=%sms (%u samples), codec=%s, mode=%s, input='%s', output='%s'\n",
             settings.jitterBufferMs, labelForFrameSetting(settings.frameSizeMs).c_str(),
             samplesForFrameSetting(settings.frameSizeMs),
             settings.rawPcm ? "pcm" : "opus",
             settings.loopbackMode ? "loopback" : "recording",
             settings.inputDeviceName.c_str(), settings.outputDeviceName.c_str());

    // Initialize components — use saved capture mode from settings
    AudioCapture capture;
    std::wstring initDeviceId;
    {
        // Find saved input device ID if set
        if (!settings.inputDeviceName.empty()) {
            for (const auto& d : AudioCapture::getDevices(settings.loopbackMode)) {
                std::string name;
                for (wchar_t wc : d.name) name += (wc < 128 ? (char)wc : '?');
                if (name == settings.inputDeviceName) { initDeviceId = d.id; break; }
            }
        }
        if (!capture.initialize(initDeviceId, settings.loopbackMode)) {
            // Fallback to default loopback
            logPrintf("[SETTINGS] Saved device failed, falling back to default loopback\n");
            settings.loopbackMode = true;
            settings.inputDeviceName.clear();
            initDeviceId.clear();
            if (!capture.initialize(L"", true)) {
                MessageBoxA(nullptr, "Failed to initialize audio capture", "AudioBridge Error", MB_OK | MB_ICONERROR);
                CoUninitialize();
                return 1;
            }
        }
    }
    capture.setFrameSize(samplesForFrameSetting(settings.frameSizeMs));
    gui.addLogMessage(std::string("Audio capture: ") + std::to_string(capture.getSampleRate()) + " Hz, " +
                      std::to_string(capture.getChannels()) + " ch" +
                      (capture.isUsingKernelTap() ? " [Direct Kernel WaveRT Tap]" : " [Event WASAPI]"));

    OpusEncoderWrapper encoder;
    if (!encoder.initialize()) {
        MessageBoxA(nullptr, "Failed to initialize Opus encoder", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    gui.addLogMessage("Opus encoder: 48kHz stereo, 128kbps, low-delay");

    OpusDecoderWrapper decoder;
    if (!decoder.initialize()) {
        MessageBoxA(nullptr, "Failed to initialize Opus decoder", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    AudioRender render;
    if (!render.initialize()) {
        gui.addLogMessage("Warning: Default receiving device init failed");
    } else {
        render.start();
    }

    // The service lease lasts for the app lifetime, independently of phone pairing.
    VirtualMicrophone virtualMic;
    virtualMic.start();
    std::atomic<bool> useVirtualMic{false};
    const std::wstring nativeMicId = L"audiobridge:microphone";

    Network network;
    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<float> peakLevel{0.0f};
    std::atomic<float> micPeakLevel{0.0f};
    std::string clientName;

    // Media info refresh signaling
    std::atomic<bool> mediaInfoRequested{false};
    std::mutex mediaWakeMutex;
    std::condition_variable mediaWakeCv;

    std::atomic<bool> useRawPcm{settings.rawPcm};

    // Set up GUI callbacks
    gui.setJitterBufferMs(settings.jitterBufferMs);
    gui.setFrameSizeMs(settings.frameSizeMs);
    gui.setRawPcm(settings.rawPcm);

    auto syncClientSettings = [&]() {
        network.sendSettings(settings.jitterBufferMs, settings.frameSizeMs,
                             (uint16_t)samplesForFrameSetting(settings.frameSizeMs),
                             settings.rawPcm);
    };

    gui.setJitterChangeCallback([&](int bufferMs) {
        gui.addLogMessage("Jitter buffer target: " + std::to_string(bufferMs) + " ms");
        settings.jitterBufferMs = bufferMs;
        saveSettings(settings);
        syncClientSettings();
    });

    gui.setFrameSizeChangeCallback([&](int frameSizeMs) {
        uint32_t samples = samplesForFrameSetting(frameSizeMs);
        gui.addLogMessage("Frame size: " + labelForFrameSetting(frameSizeMs) + " ms (" + std::to_string(samples) + " samples)");
        settings.frameSizeMs = frameSizeMs;
        saveSettings(settings);
        capture.setFrameSize(samples);
        syncClientSettings();
    });

    gui.setCodecChangeCallback([&](bool rawPcm) {
        settings.rawPcm = rawPcm;
        useRawPcm.store(rawPcm, std::memory_order_relaxed);
        saveSettings(settings);
        gui.addLogMessage(rawPcm ? "Streaming codec: Raw 16-bit PCM (0ms lookahead)" : "Streaming codec: Opus Low-Delay");
        syncClientSettings();
    });

    gui.setPairApproveCallback([&](const std::string& clientId) {
        network.approvePeer(clientId);
        gui.addLogMessage("Approved peer: " + clientId);
        gui.updateApprovedPeers(network.getApprovedPeers());
    });

    gui.setPairDenyCallback([&](const std::string& clientId) {
        network.rejectPeer(clientId);
        gui.addLogMessage("Denied peer: " + clientId);
    });

    gui.setRevokeCallback([&](const std::string& clientId) {
        network.revokePeer(clientId);
        gui.addLogMessage("Revoked peer: " + clientId);
        gui.updateApprovedPeers(network.getApprovedPeers());
    });

    network.setCallbacks(
        [&](const std::string& name) {
            clientName = name;
            packetsSent = 0;
            bytesSent = 0;
            gui.addLogMessage("Client connected: " + name + " (" + network.getClientAddress() + ")" +
                              (network.isVoiceQosActive() ? " [Wi-Fi AC_VO]" : ""));
            gui.updateApprovedPeers(network.getApprovedPeers());
            // Send current settings to client
            syncClientSettings();
        },
        [&]() {
            gui.addLogMessage("Client disconnected: " + clientName);
            clientName.clear();
        },
        [&](bool paused) {
            gui.addLogMessage(std::string("Stream ") + (paused ? "PAUSED" : "RESUMED"));
        },
        [&](const uint8_t* opusData, int opusLen) {
            float pcmOutput[960];
            int frames = decoder.decode(opusData, opusLen, pcmOutput, 960);
            if (frames > 0) {
                if (useVirtualMic.load()) virtualMic.pushAudio(pcmOutput, frames);
                else render.pushAudio(pcmOutput, frames);
                // Track mic peak level for UI meter
                float peak = 0.0f;
                for (int i = 0; i < frames; i++) {
                    float abs = pcmOutput[i] < 0 ? -pcmOutput[i] : pcmOutput[i];
                    if (abs > peak) peak = abs;
                }
                float prev = micPeakLevel.load();
                micPeakLevel.store(peak > prev ? peak : prev * 0.85f + peak * 0.15f);
            }
        },
        [&](const std::string& clientId, const std::string& name, const std::string& code) {
            if (clientId.empty()) { gui.clearPairRequest(); return; }
            gui.addLogMessage("Pair request from: " + name + " (ID: " + clientId + ")");
            gui.showPairRequest(clientId, name, code);
        },
        [&](uint8_t cmd) {
            const char* cmdName = "";
            switch (cmd) {
                case CTRL_MEDIA_PLAY_PAUSE: cmdName = "Play/Pause"; break;
                case CTRL_MEDIA_NEXT:       cmdName = "Next Track"; break;
                case CTRL_MEDIA_PREV:       cmdName = "Prev Track"; break;
            }
            logPrintf("[MEDIA] cmd=0x%02X name=%s\n", cmd, cmdName);
            try {
                thread_local bool comInit = false;
                if (!comInit) {
                    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    comInit = true;
                }
                auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                auto session = manager.GetCurrentSession();
                if (!session) {
                    logPrintf("[MEDIA] No active media session found\n");
                    gui.addLogMessage("Media: No active session");
                    return;
                }
                bool ok = false;
                switch (cmd) {
                    case CTRL_MEDIA_PLAY_PAUSE:
                        ok = session.TryTogglePlayPauseAsync().get();
                        break;
                    case CTRL_MEDIA_NEXT:
                        ok = session.TrySkipNextAsync().get();
                        break;
                    case CTRL_MEDIA_PREV:
                        ok = session.TrySkipPreviousAsync().get();
                        break;
                }
                logPrintf("[MEDIA] SMTC %s: %s\n", cmdName, ok ? "OK" : "FAILED");
                gui.addLogMessage(std::string("Media: ") + cmdName + (ok ? " OK" : " failed"));
            } catch (const winrt::hresult_error& ex) {
                logPrintf("[MEDIA] SMTC error: 0x%08X\n", (unsigned)ex.code());
                gui.addLogMessage("Media: error");
            }
        },
        [&]() {
            mediaInfoRequested.store(true);
            mediaWakeCv.notify_one();
        }
    );

    if (!network.initialize(computerName)) {
        MessageBoxA(nullptr, "Failed to initialize network", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    gui.addLogMessage("Network ready: discovery :4011, stream :4012");
    gui.addLogMessage("Server MAC: " + network.getServerMac());

    gui.updateApprovedPeers(network.getApprovedPeers());

    // Device management state
    std::wstring currentDeviceId = initDeviceId;
    std::atomic<bool> deviceInvalidated{false};
    std::wstring currentOutDeviceId;
    std::atomic<bool> outDeviceInvalidated{false};
    bool loopbackMode = settings.loopbackMode;

    // Capture callback
    uint8_t opusBuf[4000];
    int16_t pcmBuf[960 * 2];
    auto lastAudioSent = std::chrono::steady_clock::now();

    AudioCapture::FrameCallback captureCallback = [&](const float* data, uint32_t frameCount) {
        if (!network.isConnected() || network.isPaused()) return;

        // Track peak level
        for (uint32_t i = 0; i < frameCount * 2; i++) {
            float abs = data[i] < 0 ? -data[i] : data[i];
            float current = peakLevel.load(std::memory_order_relaxed);
            if (abs > current) peakLevel.store(abs, std::memory_order_relaxed);
        }

        if (useRawPcm.load(std::memory_order_relaxed)) {
            // Zero-lookahead Raw 16-bit Stereo PCM mode.
            // Sub-slice into <= 240-frame (960-byte) packets so UDP datagrams never fragment over Wi-Fi MTU.
            const uint32_t totalSamples = frameCount * 2;
            for (uint32_t i = 0; i < totalSamples; ++i) {
                float s = data[i];
                if (s > 1.0f) s = 1.0f;
                else if (s < -1.0f) s = -1.0f;
                int32_t v = static_cast<int32_t>(std::lrintf(s * 32767.0f));
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                pcmBuf[i] = static_cast<int16_t>(v);
            }

            uint32_t offsetFrames = 0;
            while (offsetFrames < frameCount) {
                uint32_t subFrames = frameCount - offsetFrames;
                if (subFrames > 240) subFrames = 240;
                network.sendAudioPcm(pcmBuf + offsetFrames * 2, (int)subFrames);
                offsetFrames += subFrames;
                packetsSent++;
                bytesSent += subFrames * 4 + 16;
            }
            lastAudioSent = std::chrono::steady_clock::now();
        } else {
            int encoded = encoder.encode(data, frameCount, opusBuf, sizeof(opusBuf));
            if (encoded > 0) {
                network.sendAudio(opusBuf, encoded);
                lastAudioSent = std::chrono::steady_clock::now();
                packetsSent++;
                bytesSent += encoded + 16;
            }
        }
    };

    // Helper: refresh device lists in GUI
    auto refreshDeviceList = [&]() {
        auto devs = AudioCapture::getDevices(loopbackMode);
        std::vector<std::pair<std::wstring, std::wstring>> devList;
        for (const auto& d : devs) {
            devList.push_back({d.id, d.name});
        }
        gui.updateDevices(devList, currentDeviceId);
    };

    auto refreshOutDeviceList = [&]() {
        auto devs = AudioRender::getDevices();
        std::vector<std::pair<std::wstring, std::wstring>> devList;
        devList.push_back({nativeMicId, virtualMic.status() == VirtualMicrophone::Status::Ready
            ? L"AudioBridge Microphone" : L"AudioBridge Microphone (unavailable)"});
        for (const auto& d : devs) {
            devList.push_back({d.first, d.second});
        }
        gui.updateOutDevices(devList, currentOutDeviceId);
    };

    // Helper: reinitialize audio capture with error recovery
    auto reinitAudio = [&](const std::wstring& deviceId) -> bool {
        logPrintf("[REINIT] stop+cleanup\n");
        capture.stop();
        capture.cleanup();

        logPrintf("[REINIT] initialize(loopback=%d)\n", loopbackMode);
        if (!capture.initialize(deviceId, loopbackMode)) {
            gui.addLogMessage("ERROR: Failed to init device, trying default...");
            if (!deviceId.empty() && capture.initialize(L"", loopbackMode)) {
                currentDeviceId = L"";
                gui.addLogMessage("Fell back to default audio device");
            } else {
                gui.addLogMessage("ERROR: No audio device available");
                refreshDeviceList();
                return false;
            }
        }

        logPrintf("[REINIT] initialized OK: %u Hz, %u ch (kernelTap=%d)\n",
                  capture.getSampleRate(), capture.getChannels(), capture.isUsingKernelTap() ? 1 : 0);
        gui.addLogMessage(std::string("Audio device: ") + std::to_string(capture.getSampleRate()) + " Hz, " +
                          std::to_string(capture.getChannels()) + " ch" +
                          (capture.isUsingKernelTap() ? " [Direct Kernel Tap]" : ""));

        capture.setFrameSize(samplesForFrameSetting(settings.frameSizeMs));
        logPrintf("[REINIT] starting capture...\n");
        if (!capture.start(captureCallback)) {
            logPrintf("[REINIT] start FAILED\n");
            gui.addLogMessage("ERROR: Failed to start audio capture");
            refreshDeviceList();
            return false;
        }

        logPrintf("[REINIT] capture started OK\n");
        refreshDeviceList();
        return true;
    };

    // Helper: reinitialize audio render with error recovery
    auto reinitOutAudio = [&](const std::wstring& deviceId) -> bool {
        useVirtualMic = deviceId == nativeMicId;
        render.stop();
        render.cleanup();

        if (useVirtualMic.load()) {
            gui.addLogMessage(virtualMic.status() == VirtualMicrophone::Status::Ready
                ? "Phone mic routes directly to AudioBridge Microphone. Select it as the input in your other apps."
                : "AudioBridge Microphone unavailable. Install the signed microphone driver/service, or select a receiving device.");
            refreshOutDeviceList();
            return virtualMic.status() == VirtualMicrophone::Status::Ready;
        }

        if (!render.initialize(deviceId)) {
            gui.addLogMessage("ERROR: Failed to init receiving device, trying default...");
            if (!deviceId.empty() && render.initialize(L"")) {
                currentOutDeviceId = L"";
                gui.addLogMessage("Fell back to default receiving device");
            } else {
                gui.addLogMessage("ERROR: No receiving device available");
                refreshOutDeviceList();
                return false;
            }
        }

        gui.addLogMessage("Receiving device switched");

        if (!render.start()) {
            gui.addLogMessage("ERROR: Failed to start receiving device");
            refreshOutDeviceList();
            return false;
        }

        refreshOutDeviceList();
        return true;
    };

    // Set device invalidation callbacks
    capture.setOnDeviceInvalidated([&]() {
        deviceInvalidated.store(true);
    });

    render.setOnDeviceInvalidated([&]() {
        outDeviceInvalidated.store(true);
    });

    // Wire GUI device change callbacks
    gui.setDeviceChangeCallback([&](const std::wstring& deviceId) {
        currentDeviceId = deviceId;
        gui.addLogMessage("Switching sending device...");
        reinitAudio(deviceId);
        settings.inputDeviceName.clear();
        for (const auto& d : AudioCapture::getDevices(loopbackMode)) {
            if (d.id == deviceId) {
                for (wchar_t wc : d.name) settings.inputDeviceName += (wc < 128 ? (char)wc : '?');
                break;
            }
        }
        saveSettings(settings);
    });

    gui.setOutDeviceChangeCallback([&](const std::wstring& deviceId) {
        currentOutDeviceId = deviceId;
        settings.preferNativeMic = deviceId == nativeMicId;
        gui.addLogMessage("Switching receiving device...");
        reinitOutAudio(deviceId);
        if (deviceId != nativeMicId) settings.outputDeviceName.clear();
        for (const auto& [devId, devName] : AudioRender::getDevices()) {
            if (devId == deviceId) {
                for (wchar_t wc : devName) settings.outputDeviceName += (wc < 128 ? (char)wc : '?');
                break;
            }
        }
        saveSettings(settings);
    });

    gui.setCaptureModeCallback([&](bool loopback) {
        logPrintf("[MODE] Switching capture mode to %s\n", loopback ? "loopback" : "recording");
        loopbackMode = loopback;
        currentDeviceId = L"";
        settings.loopbackMode = loopback;
        settings.inputDeviceName.clear();
        saveSettings(settings);
        gui.addLogMessage(loopback ? "Switched to loopback mode" : "Switched to recording device mode");
        logPrintf("[MODE] Calling reinitAudio...\n");
        bool ok = reinitAudio(L"");
        logPrintf("[MODE] reinitAudio returned %s\n", ok ? "true" : "false");
    });

    // Apply saved jitter buffer
    gui.setJitterBufferMs(settings.jitterBufferMs);

    // Populate device lists
    refreshDeviceList();
    refreshOutDeviceList();

    // Restore saved output device
    if (!settings.outputDeviceName.empty()) {
        for (const auto& [devId, devName] : AudioRender::getDevices()) {
            std::string name;
            for (wchar_t wc : devName) name += (wc < 128 ? (char)wc : '?');
            if (name == settings.outputDeviceName) {
                currentOutDeviceId = devId;
                reinitOutAudio(devId);
                logPrintf("[SETTINGS] Restored output device: %s\n", settings.outputDeviceName.c_str());
                break;
            }
        }
    }

    bool captureStarted = capture.start(captureCallback);

    if (!captureStarted) {
        MessageBoxA(nullptr, "Failed to start audio capture", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    gui.addLogMessage("Audio capture started. Waiting for clients...");

    // Timing state for tick callback
    auto startTime = std::chrono::steady_clock::now();
    auto lastKeepalive = startTime;
    auto lastPeakReset = startTime;
    auto lastPing = startTime;
    auto lastRateTime = startTime;
    uint64_t lastRateBytes = 0;
    double rollingKbps = 0.0;
    auto lastMicStatus = VirtualMicrophone::Status::Starting;

    // Tick callback: performs all periodic work previously in the main loop
    gui.setTickCallback([&]() {
        auto now = std::chrono::steady_clock::now();

        const auto micStatus = virtualMic.status();
        if (micStatus != lastMicStatus) {
            lastMicStatus = micStatus;
            if (micStatus == VirtualMicrophone::Status::Ready && settings.preferNativeMic) {
                currentOutDeviceId = nativeMicId;
                reinitOutAudio(nativeMicId);
            } else if (micStatus == VirtualMicrophone::Status::Unavailable) {
                gui.addLogMessage("AudioBridge Microphone unavailable (Windows error " +
                    std::to_string(virtualMic.error()) + "). " +
                    (useVirtualMic.load() ? "Mic output is silent until it reconnects." :
                     "Current receiving device remains active; the native microphone needs its signed driver and service."));
            }
            refreshOutDeviceList();
        }

        // Handle sending device invalidation
        if (deviceInvalidated.exchange(false)) {
            if (currentDeviceId.empty()) {
                gui.addLogMessage("Default sending device changed, reinitializing...");
                reinitAudio(L"");
            } else {
                gui.addLogMessage("Sending device disconnected. Select a new device.");
                capture.stop();
                capture.cleanup();
                refreshDeviceList();
            }
        }

        // Handle receiving device invalidation
        if (outDeviceInvalidated.exchange(false) && !useVirtualMic.load()) {
            if (currentOutDeviceId.empty()) {
                gui.addLogMessage("Default receiving device changed, reinitializing...");
                reinitOutAudio(L"");
            } else {
                gui.addLogMessage("Receiving device disconnected. Select a new device.");
                render.stop();
                render.cleanup();
                refreshOutDeviceList();
            }
        }

        // Send keepalive every 500ms
        auto keepaliveElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastKeepalive).count();
        auto sinceAudio = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastAudioSent).count();
        if (keepaliveElapsed >= 500 && network.isConnected() && sinceAudio >= 100) {
            network.sendKeepalive();
            lastKeepalive = now;
        }

        // Send ping every 2 seconds for RTT measurement
        auto pingElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastPing).count();
        if (pingElapsed >= 2000 && network.isConnected()) {
            network.sendPing();
            lastPing = now;
        }

        // Reset peak level every 2 seconds
        auto peakElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastPeakReset).count();
        if (peakElapsed >= 2000) {
            peakLevel.store(0.0f);
            micPeakLevel.store(0.0f);
            lastPeakReset = now;
        }

        // Compute real-time rolling bitrate every 500ms
        double rateElapsedSec = std::chrono::duration<double>(now - lastRateTime).count();
        if (rateElapsedSec >= 0.5) {
            uint64_t curBytes = bytesSent.load(std::memory_order_relaxed);
            if (curBytes >= lastRateBytes) {
                rollingKbps = ((double)(curBytes - lastRateBytes) * 8.0) / (rateElapsedSec * 1000.0);
            } else {
                rollingKbps = 0.0;
            }
            lastRateBytes = curBytes;
            lastRateTime = now;
        }

        // Update GUI stats
        ServerStats stats;
        stats.connected = network.isConnected();
        stats.paused = network.isPaused();
        stats.clientName = clientName;
        stats.clientAddress = network.getClientAddress();
        stats.packetsSent = packetsSent;
        stats.bytesSent = bytesSent;
        stats.kbps = network.isConnected() ? rollingKbps : 0.0;
        stats.peakLevel = peakLevel.load();
        stats.micPeakLevel = micPeakLevel.load();
        stats.encrypted = network.isDtlsActive();
        stats.rawPcm = gui.getRawPcm();
        stats.voiceQos = network.isVoiceQosActive();
        stats.kernelTap = capture.isUsingKernelTap();
        stats.jitterBufferMs = gui.getJitterBufferMs();
        stats.frameSizeMs = gui.getFrameSizeMs();
        stats.sequenceNum = (uint32_t)packetsSent.load();
        stats.uptimeSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startTime).count() / 1000.0;
        stats.serverName = computerName;
        stats.serverMac = network.getServerMac();
        stats.clientRttMs = network.getClientRtt();

        gui.updateStats(stats);
    });

    // Media info polling thread (must run on MTA thread, not GUI STA thread)
    std::atomic<bool> mediaThreadRunning{true};

    std::thread mediaInfoThread([&]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string lastJson;

        auto toUtf8 = [](winrt::hstring const& hs) -> std::string {
            if (hs.empty()) return "";
            int len = WideCharToMultiByte(CP_UTF8, 0, hs.c_str(), (int)hs.size(), nullptr, 0, nullptr, nullptr);
            std::string s(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, hs.c_str(), (int)hs.size(), &s[0], len, nullptr, nullptr);
            return s;
        };
        auto escJson = [](const std::string& s) -> std::string {
            std::string out;
            for (char c : s) {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else out += c;
            }
            return out;
        };

        while (mediaThreadRunning) {
            bool forced = mediaInfoRequested.exchange(false);

            if (network.isConnected()) {
                try {
                    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                    auto session = manager.GetCurrentSession();
                    std::string json;
                    if (session) {
                        auto info = session.TryGetMediaPropertiesAsync().get();
                        auto playback = session.GetPlaybackInfo();
                        auto timeline = session.GetTimelineProperties();

                        std::string title = toUtf8(info.Title());
                        std::string artist = toUtf8(info.Artist());
                        std::string album = toUtf8(info.AlbumTitle());

                        int status = 0;
                        if (playback) {
                            auto ps = playback.PlaybackStatus();
                            if (ps == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) status = 1;
                            else if (ps == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused) status = 2;
                        }

                        long long posMs = 0, durMs = 0;
                        if (timeline) {
                            posMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeline.Position()).count();
                            durMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeline.EndTime()).count();
                        }

                        json = "{\"t\":\"" + escJson(title) +
                               "\",\"a\":\"" + escJson(artist) +
                               "\",\"al\":\"" + escJson(album) +
                               "\",\"s\":" + std::to_string(status) +
                               ",\"p\":" + std::to_string(posMs) +
                               ",\"d\":" + std::to_string(durMs) + "}";
                    } else {
                        json = "{\"t\":\"\",\"a\":\"\",\"al\":\"\",\"s\":0,\"p\":0,\"d\":0}";
                    }

                    if (forced || json != lastJson) {
                        lastJson = json;
                        network.sendMediaInfo(json);
                    }
                } catch (...) {}
            }

            // Wait up to 2 seconds, but wake immediately on request
            std::unique_lock<std::mutex> lock(mediaWakeMutex);
            mediaWakeCv.wait_for(lock, std::chrono::seconds(2), [&] {
                return mediaInfoRequested.load() || !mediaThreadRunning.load();
            });
        }
        CoUninitialize();
    });

    // Run webview event loop (blocks until window closed)
    gui.run();

    // Cleanup
    mediaThreadRunning = false;
    mediaWakeCv.notify_one();
    mediaInfoThread.join();
    render.stop();
    capture.stop();
    network.shutdown();
    virtualMic.stop();
    CoUninitialize();
    timeEndPeriod(1);

    return 0;
}
