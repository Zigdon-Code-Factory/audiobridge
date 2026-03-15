#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <string>
#include <fstream>

#include "audio_capture.h"
#include "opus_encoder.h"
#include "network.h"
#include "gui.h"

#pragma comment(lib, "dbghelp.lib")

static std::atomic<bool> g_running{true};

static std::string getCrashLogPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        path = path.substr(0, lastSlash + 1);
    return path + "audiobridge_crash.log";
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetUnhandledExceptionFilter(CrashHandler);

    // Initialize COM for WASAPI
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
        MessageBoxA(nullptr, "Failed to create GUI window", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    gui.addLogMessage("AudioBridge Server v1.0 starting...");

    // Initialize components
    AudioCapture capture;
    if (!capture.initialize()) {
        MessageBoxA(nullptr, "Failed to initialize audio capture", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    gui.addLogMessage("Audio capture initialized: " + std::to_string(capture.getSampleRate()) + " Hz, " +
                      std::to_string(capture.getChannels()) + " ch");

    OpusEncoderWrapper encoder;
    if (!encoder.initialize()) {
        MessageBoxA(nullptr, "Failed to initialize Opus encoder", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    gui.addLogMessage("Opus encoder: 48kHz stereo, 128kbps, low-delay");

    Network network;
    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<float> peakLevel{0.0f};
    std::string clientName;

    // Set up GUI callbacks
    gui.setJitterChangeCallback([&](int bufferMs) {
        gui.addLogMessage("Jitter buffer target: " + std::to_string(bufferMs) + " ms");
        // TODO: Send jitter config to connected client via control message
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

    network.setCallbacks(
        [&](const std::string& name) {
            clientName = name;
            packetsSent = 0;
            bytesSent = 0;
            gui.addLogMessage("Client connected: " + name + " (" + network.getClientAddress() + ")");
            gui.updateApprovedPeers(network.getApprovedPeers());
        },
        [&]() {
            gui.addLogMessage("Client disconnected: " + clientName);
            clientName.clear();
        },
        [&](bool paused) {
            gui.addLogMessage(std::string("Stream ") + (paused ? "PAUSED" : "RESUMED"));
        },
        [&](const std::string& clientId, const std::string& name) {
            gui.addLogMessage("Pair request from: " + name + " (ID: " + clientId + ")");
            gui.showPairRequest(clientId, name);
        }
    );

    if (!network.initialize(computerName)) {
        MessageBoxA(nullptr, "Failed to initialize network", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    gui.addLogMessage("Network ready: discovery :4011, stream :4012");
    gui.addLogMessage("Server MAC: " + network.getServerMac());

    // Load initial approved peers into GUI
    gui.updateApprovedPeers(network.getApprovedPeers());

    // Start capture — encode and send audio frames
    uint8_t opusBuf[4000];
    auto lastAudioSent = std::chrono::steady_clock::now();

    bool captureStarted = capture.start([&](const float* data, uint32_t frameCount) {
        if (!network.isConnected() || network.isPaused()) return;

        // Track peak level
        for (uint32_t i = 0; i < frameCount * 2; i++) {
            float abs = data[i] < 0 ? -data[i] : data[i];
            float current = peakLevel.load();
            if (abs > current) peakLevel.store(abs);
        }

        int encoded = encoder.encode(data, frameCount, opusBuf, sizeof(opusBuf));
        if (encoded > 0) {
            network.sendAudio(opusBuf, encoded);
            lastAudioSent = std::chrono::steady_clock::now();
            packetsSent++;
            bytesSent += encoded + 16;
        }
    });

    if (!captureStarted) {
        MessageBoxA(nullptr, "Failed to start audio capture", "AudioBridge Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    gui.addLogMessage("Audio capture started. Waiting for clients...");

    auto startTime = std::chrono::steady_clock::now();
    auto lastKeepalive = startTime;
    auto lastPeakReset = startTime;

    // Main loop — GUI message pump + status updates + keepalive
    while (g_running) {
        // Process window messages
        if (!gui.processMessages()) {
            g_running = false;
            break;
        }

        auto now = std::chrono::steady_clock::now();

        // Send keepalive every 500ms
        auto keepaliveElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastKeepalive).count();
        auto sinceAudio = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastAudioSent).count();
        if (keepaliveElapsed >= 500 && network.isConnected() && sinceAudio >= 100) {
            network.sendKeepalive();
            lastKeepalive = now;
        }

        // Reset peak level every 2 seconds
        auto peakElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastPeakReset).count();
        if (peakElapsed >= 2000) {
            peakLevel.store(0.0f);
            lastPeakReset = now;
        }

        // Update GUI stats
        ServerStats stats;
        stats.connected = network.isConnected();
        stats.paused = network.isPaused();
        stats.clientName = clientName;
        stats.clientAddress = network.getClientAddress();
        stats.packetsSent = packetsSent;
        stats.bytesSent = bytesSent;
        stats.kbps = (double)bytesSent * 8.0 / 1000.0;
        stats.peakLevel = peakLevel.load();
        stats.jitterBufferMs = gui.getJitterBufferMs();
        stats.sequenceNum = (uint32_t)packetsSent.load();
        stats.uptimeSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startTime).count() / 1000.0;
        stats.serverName = computerName;
        stats.serverMac = network.getServerMac();

        gui.updateStats(stats);

        Sleep(5);  // ~200fps message loop, actual paint is timer-driven at ~5fps
    }

    capture.stop();
    network.shutdown();
    CoUninitialize();

    return 0;
}
