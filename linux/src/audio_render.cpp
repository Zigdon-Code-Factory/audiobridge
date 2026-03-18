#include "audio_render.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <cstdio>
#include <cstring>

static std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    std::string s;
    s.reserve(ws.size());
    for (wchar_t wc : ws) {
        if (wc < 0x80) s += (char)wc;
        else s += '?';
    }
    return s;
}

static std::wstring utf8ToWide(const std::string& s) {
    std::wstring ws;
    ws.reserve(s.size());
    for (char c : s) ws += (wchar_t)(unsigned char)c;
    return ws;
}

AudioRender::AudioRender() {}

AudioRender::~AudioRender() {
    stop();
    cleanup();
}

static void sinkInfoCallback(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
    if (eol > 0 || !info) return;
    auto* devices = static_cast<std::vector<std::pair<std::wstring, std::wstring>>*>(userdata);
    devices->push_back({utf8ToWide(info->name), utf8ToWide(info->description)});
}

std::vector<std::pair<std::wstring, std::wstring>> AudioRender::getDevices() {
    std::vector<std::pair<std::wstring, std::wstring>> devices;

    pa_mainloop* ml = pa_mainloop_new();
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "audiobridge-enum");

    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return devices;
    }

    while (true) {
        pa_mainloop_iterate(ml, 1, nullptr);
        auto state = pa_context_get_state(ctx);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state)) {
            pa_context_unref(ctx);
            pa_mainloop_free(ml);
            return devices;
        }
    }

    pa_operation* op = pa_context_get_sink_info_list(ctx, sinkInfoCallback, &devices);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml, 1, nullptr);
    }
    pa_operation_unref(op);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    return devices;
}

bool AudioRender::initialize(const std::wstring& deviceId) {
    if (!deviceId.empty()) {
        pulseDeviceName_ = wideToUtf8(deviceId);
    } else {
        pulseDeviceName_ = ""; // default sink
    }
    return true;
}

void AudioRender::cleanup() {
    stop();
}

bool AudioRender::start() {
    if (running_) return false;
    running_ = true;
    thread_ = std::thread(&AudioRender::renderThread, this);
    return true;
}

void AudioRender::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioRender::pushAudio(const float* data, uint32_t frameCount) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    audioBuffer_.insert(audioBuffer_.end(), data, data + frameCount);
}

void AudioRender::renderThread() {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = 48000;
    ss.channels = 1; // mono from mic decoder

    pa_buffer_attr bufattr;
    memset(&bufattr, 0, sizeof(bufattr));
    bufattr.maxlength = (uint32_t)-1;
    bufattr.tlength = 480 * sizeof(float); // 10ms target latency
    bufattr.prebuf = (uint32_t)-1;
    bufattr.minreq = (uint32_t)-1;

    int error = 0;
    const char* device = pulseDeviceName_.empty() ? nullptr : pulseDeviceName_.c_str();

    pa_simple* s = pa_simple_new(
        nullptr,
        "AudioBridge",
        PA_STREAM_PLAYBACK,
        device,
        "Audio Render",
        &ss,
        nullptr,
        &bufattr,
        &error
    );

    if (!s) {
        printf("pa_simple_new() playback failed: %s\n", pa_strerror(error));
        running_ = false;
        return;
    }

    const int chunkSize = 480; // 10ms at 48kHz mono
    float buf[chunkSize];

    while (running_) {
        bool hasData = false;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            if (audioBuffer_.size() >= (size_t)chunkSize) {
                memcpy(buf, audioBuffer_.data(), chunkSize * sizeof(float));
                audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.begin() + chunkSize);
                hasData = true;
            }
        }

        if (hasData) {
            if (pa_simple_write(s, buf, chunkSize * sizeof(float), &error) < 0) {
                printf("pa_simple_write() failed: %s\n", pa_strerror(error));
                break;
            }
        } else {
            // No data — write silence to keep stream alive
            memset(buf, 0, sizeof(buf));
            if (pa_simple_write(s, buf, chunkSize * sizeof(float), &error) < 0) {
                break;
            }
        }
    }

    pa_simple_drain(s, &error);
    pa_simple_free(s);
}
