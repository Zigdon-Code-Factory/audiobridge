#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>

#include "audio_capture.h"
#include "opus_encoder.h"
#include "network.h"

static std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int main() {
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    printf("=== AudioBridge Server v1.0 ===\n\n");

    // Initialize COM for WASAPI
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("COM init failed: 0x%08lx\n", hr);
        return 1;
    }

    // Get computer name for server identity
    char computerName[256] = "AudioBridge";
    DWORD nameLen = sizeof(computerName);
    GetComputerNameA(computerName, &nameLen);

    // Initialize components
    AudioCapture capture;
    if (!capture.initialize()) {
        printf("Failed to initialize audio capture\n");
        CoUninitialize();
        return 1;
    }

    OpusEncoderWrapper encoder;
    if (!encoder.initialize()) {
        printf("Failed to initialize Opus encoder\n");
        CoUninitialize();
        return 1;
    }

    Network network;
    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> bytesSent{0};
    std::string clientName;

    network.setCallbacks(
        [&](const std::string& name) {
            clientName = name;
            packetsSent = 0;
            bytesSent = 0;
            printf("\n>> Client connected: %s (%s)\n",
                   name.c_str(), network.getClientAddress().c_str());
        },
        [&]() {
            printf("\n>> Client disconnected\n");
            clientName.clear();
        },
        [&](bool paused) {
            printf("\n>> Stream %s\n", paused ? "PAUSED" : "RESUMED");
        }
    );

    if (!network.initialize(computerName)) {
        printf("Failed to initialize network\n");
        CoUninitialize();
        return 1;
    }

    // Start capture — encode and send audio frames
    uint8_t opusBuf[4000];
    auto lastAudioSent = std::chrono::steady_clock::now();

    bool captureStarted = capture.start([&](const float* data, uint32_t frameCount) {
        if (!network.isConnected() || network.isPaused()) return;

        int encoded = encoder.encode(data, frameCount, opusBuf, sizeof(opusBuf));
        if (encoded > 0) {
            network.sendAudio(opusBuf, encoded);
            lastAudioSent = std::chrono::steady_clock::now();
            packetsSent++;
            bytesSent += encoded + 16;
        }
    });

    if (!captureStarted) {
        printf("Failed to start audio capture\n");
        CoUninitialize();
        return 1;
    }

    printf("\nServer '%s' ready. Waiting for clients...\n", computerName);
    printf("Press Ctrl+C to exit.\n\n");

    // Main loop — status display and keepalive
    auto lastKeepalive = std::chrono::steady_clock::now();
    auto lastStatus = lastKeepalive;

    while (g_running) {
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

        // Print status every 2 seconds
        auto statusElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastStatus).count();
        if (statusElapsed >= 2000) {
            if (network.isConnected()) {
                double kbps = (double)bytesSent * 8.0 / 1000.0;
                printf("\r  Streaming to %s | %llu pkts | %.0f kbps total   ",
                       clientName.c_str(), (unsigned long long)packetsSent.load(), kbps);
            } else {
                printf("\r  Waiting for client...                              ");
            }
            fflush(stdout);
            lastStatus = now;
        }

        Sleep(10);
    }

    printf("\n\nShutting down...\n");
    capture.stop();
    network.shutdown();
    CoUninitialize();

    return 0;
}
