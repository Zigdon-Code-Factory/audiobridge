#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <signal.h>

#include "audio_capture.h"
#include "audio_render.h"
#include "opus_encoder.h"
#include "opus_decoder.h"
#include "network.h"
#include "gui.h"

// --- macOS media control via osascript ---

static bool osascriptRun(const std::string& script) {
    std::string cmd = "osascript -e '" + script + "' 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

static std::string osascriptGet(const std::string& script) {
    std::string cmd = "osascript -e '" + script + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[1024];
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static void mediaPlayPause() {
    // Try Spotify first, then Music (iTunes), then generic media key
    if (system("pgrep -x Spotify >/dev/null 2>&1") == 0) {
        osascriptRun("tell application \"Spotify\" to playpause");
    } else if (system("pgrep -x Music >/dev/null 2>&1") == 0) {
        osascriptRun("tell application \"Music\" to playpause");
    } else {
        // Simulate media key press via CGEvent
        system("osascript -e 'tell application \"System Events\" to key code 16 using {command down}' 2>/dev/null");
    }
}

static void mediaNext() {
    if (system("pgrep -x Spotify >/dev/null 2>&1") == 0) {
        osascriptRun("tell application \"Spotify\" to next track");
    } else if (system("pgrep -x Music >/dev/null 2>&1") == 0) {
        osascriptRun("tell application \"Music\" to next track");
    }
}

static void mediaPrev() {
    if (system("pgrep -x Spotify >/dev/null 2>&1") == 0) {
        osascriptRun("tell application \"Spotify\" to previous track");
    } else if (system("pgrep -x Music >/dev/null 2>&1") == 0) {
        osascriptRun("tell application \"Music\" to previous track");
    }
}

static std::string getMediaInfo() {
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

    std::string title, artist, album;
    int status = 0; // 0=stopped, 1=playing, 2=paused
    long long posMs = 0, durMs = 0;

    if (system("pgrep -x Spotify >/dev/null 2>&1") == 0) {
        std::string state = osascriptGet("tell application \"Spotify\" to player state as string");
        if (state == "playing") status = 1;
        else if (state == "paused") status = 2;

        if (status > 0) {
            title = osascriptGet("tell application \"Spotify\" to name of current track");
            artist = osascriptGet("tell application \"Spotify\" to artist of current track");
            album = osascriptGet("tell application \"Spotify\" to album of current track");
            std::string pos = osascriptGet("tell application \"Spotify\" to player position");
            std::string dur = osascriptGet("tell application \"Spotify\" to duration of current track");
            try { posMs = (long long)(std::stod(pos) * 1000); } catch (...) {}
            try { durMs = std::stoll(dur); } catch (...) {} // Spotify returns ms
        }
    } else if (system("pgrep -x Music >/dev/null 2>&1") == 0) {
        std::string state = osascriptGet("tell application \"Music\" to player state as string");
        if (state == "playing") status = 1;
        else if (state == "paused") status = 2;

        if (status > 0) {
            title = osascriptGet("tell application \"Music\" to name of current track");
            artist = osascriptGet("tell application \"Music\" to artist of current track");
            album = osascriptGet("tell application \"Music\" to album of current track");
            std::string pos = osascriptGet("tell application \"Music\" to player position");
            std::string dur = osascriptGet("tell application \"Music\" to duration of current track");
            try { posMs = (long long)(std::stod(pos) * 1000); } catch (...) {}
            try { durMs = (long long)(std::stod(dur) * 1000); } catch (...) {}
        }
    }

    return "{\"t\":\"" + escJson(title) +
           "\",\"a\":\"" + escJson(artist) +
           "\",\"al\":\"" + escJson(album) +
           "\",\"s\":" + std::to_string(status) +
           ",\"p\":" + std::to_string(posMs) +
           ",\"d\":" + std::to_string(durMs) + "}";
}

// --- Main ---

int main(int argc, char* argv[]) {
    signal(SIGINT, [](int) { exit(0); });
    signal(SIGTERM, [](int) { exit(0); });

    printf("AudioBridge macOS Server starting...\n");

    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::string computerName(hostname);
    // Strip .local suffix if present
    size_t dotLocal = computerName.find(".local");
    if (dotLocal != std::string::npos) computerName = computerName.substr(0, dotLocal);
    printf("Server name: %s\n", computerName.c_str());

    ServerGui gui;
    if (!gui.initialize("AudioBridge")) {
        fprintf(stderr, "Failed to initialize GUI\n");
        return 1;
    }

    AudioCapture capture;
    AudioRender render;
    OpusEncoderWrapper encoder;
    OpusDecoderWrapper decoder;
    Network network;

    if (!encoder.initialize()) { fprintf(stderr, "Failed to init encoder\n"); return 1; }
    if (!decoder.initialize()) { fprintf(stderr, "Failed to init decoder\n"); return 1; }
    if (!capture.initialize()) { fprintf(stderr, "Failed to init capture\n"); return 1; }
    if (!render.initialize()) { fprintf(stderr, "Failed to init render\n"); return 1; }
    render.start();

    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<float> peakLevel{0.0f};
    std::string clientName;

    std::atomic<bool> mediaInfoRequested{false};
    std::mutex mediaWakeMutex;
    std::condition_variable mediaWakeCv;

    gui.setJitterChangeCallback([&](int bufferMs) {
        gui.addLogMessage("Jitter buffer target: " + std::to_string(bufferMs) + " ms");
    });
    gui.setPairApproveCallback([&](const std::string& clientId) {
        network.approvePeer(clientId);
        gui.updateApprovedPeers(network.getApprovedPeers());
    });
    gui.setPairDenyCallback([&](const std::string& clientId) {
        network.rejectPeer(clientId);
    });
    gui.setRevokeCallback([&](const std::string& clientId) {
        network.revokePeer(clientId);
        gui.updateApprovedPeers(network.getApprovedPeers());
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
        [&](const uint8_t* opusData, int opusLen) {
            float pcmOutput[960];
            int frames = decoder.decode(opusData, opusLen, pcmOutput, 960);
            if (frames > 0) render.pushAudio(pcmOutput, frames);
        },
        [&](const std::string& clientId, const std::string& name) {
            gui.addLogMessage("Pair request from: " + name + " (ID: " + clientId + ")");
            gui.showPairRequest(clientId, name);
        },
        [&](uint8_t cmd) {
            const char* cmdName = "";
            switch (cmd) {
                case CTRL_MEDIA_PLAY_PAUSE: cmdName = "Play/Pause"; mediaPlayPause(); break;
                case CTRL_MEDIA_NEXT:       cmdName = "Next Track"; mediaNext(); break;
                case CTRL_MEDIA_PREV:       cmdName = "Prev Track"; mediaPrev(); break;
            }
            gui.addLogMessage(std::string("Media: ") + cmdName);
        },
        [&]() {
            mediaInfoRequested.store(true);
            mediaWakeCv.notify_one();
        }
    );

    if (!network.initialize(computerName)) {
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }
    gui.addLogMessage("Network ready: discovery :4011, stream :4012");
    gui.addLogMessage("Server MAC: " + network.getServerMac());
    gui.updateApprovedPeers(network.getApprovedPeers());

    // Audio capture
    uint8_t opusBuf[4000];
    auto lastAudioSent = std::chrono::steady_clock::now();

    AudioCapture::FrameCallback captureCallback = [&](const float* data, uint32_t frameCount) {
        if (!network.isConnected() || network.isPaused()) return;

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
    };

    if (!capture.start(captureCallback)) {
        fprintf(stderr, "Failed to start audio capture\n");
        return 1;
    }
    gui.addLogMessage("Audio capture started. Waiting for clients...");

    auto startTime = std::chrono::steady_clock::now();
    auto lastKeepalive = startTime;
    auto lastPeakReset = startTime;

    gui.setTickCallback([&]() {
        auto now = std::chrono::steady_clock::now();

        auto keepaliveElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastKeepalive).count();
        auto sinceAudio = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastAudioSent).count();
        if (keepaliveElapsed >= 500 && network.isConnected() && sinceAudio >= 100) {
            network.sendKeepalive();
            lastKeepalive = now;
        }

        auto peakElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastPeakReset).count();
        if (peakElapsed >= 2000) {
            peakLevel.store(0.0f);
            lastPeakReset = now;
        }

        ServerStats stats;
        stats.connected = network.isConnected();
        stats.paused = network.isPaused();
        stats.clientName = clientName;
        stats.clientAddress = network.getClientAddress();
        stats.packetsSent = packetsSent;
        stats.bytesSent = bytesSent;
        stats.kbps = bytesSent > 0 ?
            (bytesSent * 8.0 / std::chrono::duration_cast<std::chrono::milliseconds>(
                now - startTime).count()) : 0.0;
        stats.peakLevel = peakLevel;
        stats.jitterBufferMs = gui.getJitterBufferMs();
        stats.sequenceNum = packetsSent;
        stats.uptimeSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startTime).count() / 1000.0;
        stats.serverName = computerName;
        stats.serverMac = network.getServerMac();

        gui.updateStats(stats);
    });

    // Media info polling thread
    std::atomic<bool> mediaThreadRunning{true};

    std::thread mediaInfoThread([&]() {
        std::string lastJson;
        while (mediaThreadRunning) {
            bool forced = mediaInfoRequested.exchange(false);

            if (network.isConnected()) {
                try {
                    std::string json = getMediaInfo();
                    if (forced || json != lastJson) {
                        lastJson = json;
                        network.sendMediaInfo(json);
                    }
                } catch (...) {}
            }

            std::unique_lock<std::mutex> lock(mediaWakeMutex);
            mediaWakeCv.wait_for(lock, std::chrono::seconds(2), [&] {
                return mediaInfoRequested.load() || !mediaThreadRunning.load();
            });
        }
    });

    gui.run();

    mediaThreadRunning = false;
    mediaWakeCv.notify_one();
    mediaInfoThread.join();
    render.stop();
    capture.stop();
    network.shutdown();

    printf("AudioBridge shutdown.\n");
    return 0;
}
