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

// --- MPRIS2 D-Bus media control ---
// Uses dbus-send command for simplicity (avoids linking libdbus)

static std::string getActiveMprisPlayer() {
    FILE* pipe = popen("dbus-send --session --dest=org.freedesktop.DBus "
                       "--type=method_call --print-reply /org/freedesktop/DBus "
                       "org.freedesktop.DBus.ListNames 2>/dev/null", "r");
    if (!pipe) return "";

    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    pclose(pipe);

    // Find first org.mpris.MediaPlayer2.* entry
    size_t pos = 0;
    while ((pos = output.find("org.mpris.MediaPlayer2.", pos)) != std::string::npos) {
        size_t start = pos;
        size_t end = output.find('"', start);
        if (end == std::string::npos) end = output.find('\n', start);
        if (end == std::string::npos) break;
        std::string name = output.substr(start, end - start);
        // Clean up any trailing whitespace
        while (!name.empty() && (name.back() == '"' || name.back() == '\n' || name.back() == ' '))
            name.pop_back();
        return name;
    }
    return "";
}

static bool mprisCommand(const std::string& method) {
    std::string player = getActiveMprisPlayer();
    if (player.empty()) return false;

    std::string cmd = "dbus-send --session --type=method_call --dest='" + player +
                      "' /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player." +
                      method + " 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

static std::string mprisGetMediaInfo() {
    std::string player = getActiveMprisPlayer();
    if (player.empty()) {
        return R"({"t":"","a":"","al":"","s":0,"p":0,"d":0})";
    }

    // Get metadata
    std::string cmd = "dbus-send --session --print-reply --dest='" + player +
                      "' /org/mpris/MediaPlayer2 "
                      "org.freedesktop.DBus.Properties.Get "
                      "string:'org.mpris.MediaPlayer2.Player' string:'Metadata' 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return R"({"t":"","a":"","al":"","s":0,"p":0,"d":0})";

    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);

    auto extractString = [&](const std::string& key) -> std::string {
        size_t pos = output.find(key);
        if (pos == std::string::npos) return "";
        // Find the string value after the key — look for 'string "value"'
        pos = output.find("string \"", pos + key.size());
        if (pos == std::string::npos) return "";
        pos += 8; // skip 'string "'
        size_t end = output.find('"', pos);
        if (end == std::string::npos) return "";
        return output.substr(pos, end - pos);
    };

    auto extractInt64 = [&](const std::string& key) -> long long {
        size_t pos = output.find(key);
        if (pos == std::string::npos) return 0;
        pos = output.find("int64 ", pos);
        if (pos == std::string::npos) {
            pos = output.find("uint64 ", pos);
            if (pos == std::string::npos) return 0;
            pos += 7;
        } else {
            pos += 6;
        }
        return std::stoll(output.substr(pos));
    };

    std::string title = extractString("xesam:title");
    std::string artist = extractString("xesam:artist");
    std::string album = extractString("xesam:album");
    long long lengthUs = extractInt64("mpris:length"); // microseconds

    // Get playback status
    cmd = "dbus-send --session --print-reply --dest='" + player +
          "' /org/mpris/MediaPlayer2 "
          "org.freedesktop.DBus.Properties.Get "
          "string:'org.mpris.MediaPlayer2.Player' string:'PlaybackStatus' 2>/dev/null";

    pipe = popen(cmd.c_str(), "r");
    std::string statusOutput;
    if (pipe) {
        while (fgets(buf, sizeof(buf), pipe)) statusOutput += buf;
        pclose(pipe);
    }

    int status = 0;
    if (statusOutput.find("Playing") != std::string::npos) status = 1;
    else if (statusOutput.find("Paused") != std::string::npos) status = 2;

    // Get position
    cmd = "dbus-send --session --print-reply --dest='" + player +
          "' /org/mpris/MediaPlayer2 "
          "org.freedesktop.DBus.Properties.Get "
          "string:'org.mpris.MediaPlayer2.Player' string:'Position' 2>/dev/null";

    pipe = popen(cmd.c_str(), "r");
    std::string posOutput;
    if (pipe) {
        while (fgets(buf, sizeof(buf), pipe)) posOutput += buf;
        pclose(pipe);
    }

    long long posUs = 0;
    size_t posIdx = posOutput.find("int64 ");
    if (posIdx != std::string::npos) {
        try { posUs = std::stoll(posOutput.substr(posIdx + 6)); } catch (...) {}
    }

    // Escape for JSON
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

    return "{\"t\":\"" + escJson(title) +
           "\",\"a\":\"" + escJson(artist) +
           "\",\"al\":\"" + escJson(album) +
           "\",\"s\":" + std::to_string(status) +
           ",\"p\":" + std::to_string(posUs / 1000) +
           ",\"d\":" + std::to_string(lengthUs / 1000) + "}";
}

// --- Main ---

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    printf("AudioBridge Linux Server starting...\n");

    // Get hostname for server name
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::string computerName(hostname);
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

    if (!encoder.initialize()) {
        fprintf(stderr, "Failed to initialize Opus encoder\n");
        return 1;
    }
    if (!decoder.initialize()) {
        fprintf(stderr, "Failed to initialize Opus decoder\n");
        return 1;
    }

    if (!capture.initialize()) {
        fprintf(stderr, "Failed to initialize audio capture\n");
        return 1;
    }

    if (!render.initialize()) {
        fprintf(stderr, "Failed to initialize audio render\n");
        return 1;
    }
    render.start();

    // Stats
    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<float> peakLevel{0.0f};
    std::string clientName;

    // Media info refresh signaling
    std::atomic<bool> mediaInfoRequested{false};
    std::mutex mediaWakeMutex;
    std::condition_variable mediaWakeCv;

    // GUI callbacks
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
            if (frames > 0) {
                render.pushAudio(pcmOutput, frames);
            }
        },
        [&](const std::string& clientId, const std::string& name) {
            gui.addLogMessage("Pair request from: " + name + " (ID: " + clientId + ")");
            gui.showPairRequest(clientId, name);
        },
        [&](uint8_t cmd) {
            const char* cmdName = "";
            switch (cmd) {
                case CTRL_MEDIA_PLAY_PAUSE: cmdName = "Play/Pause"; mprisCommand("PlayPause"); break;
                case CTRL_MEDIA_NEXT:       cmdName = "Next Track"; mprisCommand("Next"); break;
                case CTRL_MEDIA_PREV:       cmdName = "Prev Track"; mprisCommand("Previous"); break;
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

    // Timing
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
                    std::string json = mprisGetMediaInfo();

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

    // Run GUI event loop
    gui.run();

    // Cleanup
    mediaThreadRunning = false;
    mediaWakeCv.notify_one();
    mediaInfoThread.join();
    render.stop();
    capture.stop();
    network.shutdown();

    printf("AudioBridge shutdown.\n");
    return 0;
}
