#include "audio_capture.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <cstdio>
#include <cstring>

// Helper to convert wstring to string for PulseAudio
static std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    std::string s;
    s.reserve(ws.size());
    for (wchar_t wc : ws) {
        if (wc < 0x80) s += (char)wc;
        else {
            // Simple ASCII fallback — PA device names are ASCII anyway
            s += '?';
        }
    }
    return s;
}

static std::wstring utf8ToWide(const std::string& s) {
    std::wstring ws;
    ws.reserve(s.size());
    for (char c : s) ws += (wchar_t)(unsigned char)c;
    return ws;
}

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stop();
    cleanup();
}

// Enumerate PulseAudio monitor sources (loopback capture)
static void sourceInfoCallback(pa_context*, const pa_source_info* info, int eol, void* userdata) {
    if (eol > 0 || !info) return;
    auto* devices = static_cast<std::vector<AudioDeviceInfo>*>(userdata);

    // Include monitor sources (loopback) and regular sources
    AudioDeviceInfo dev;
    dev.id = utf8ToWide(info->name);
    dev.name = utf8ToWide(info->description);
    devices->push_back(dev);
}

std::vector<AudioDeviceInfo> AudioCapture::getDevices() {
    std::vector<AudioDeviceInfo> devices;

    pa_mainloop* ml = pa_mainloop_new();
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "audiobridge-enum");

    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return devices;
    }

    // Wait for context to be ready
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

    pa_operation* op = pa_context_get_source_info_list(ctx, sourceInfoCallback, &devices);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml, 1, nullptr);
    }
    pa_operation_unref(op);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    return devices;
}

bool AudioCapture::initialize(const std::wstring& deviceId) {
    if (!deviceId.empty()) {
        pulseDeviceName_ = wideToUtf8(deviceId);
    } else {
        // Default: use the default monitor source (system audio loopback)
        // We'll find it in start()
        pulseDeviceName_ = "";
    }
    sampleRate_ = 48000;
    channels_ = 2;
    return true;
}

void AudioCapture::cleanup() {
    stop();
}

bool AudioCapture::start(FrameCallback callback) {
    if (running_) return false;
    callback_ = callback;
    running_ = true;
    thread_ = std::thread(&AudioCapture::captureThread, this);
    return true;
}

void AudioCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// Find the monitor source for the default sink
static std::string getDefaultMonitorSource() {
    std::string monitorName;

    pa_mainloop* ml = pa_mainloop_new();
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "audiobridge-monitor");

    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return "";
    }

    while (true) {
        pa_mainloop_iterate(ml, 1, nullptr);
        auto state = pa_context_get_state(ctx);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state)) {
            pa_context_unref(ctx);
            pa_mainloop_free(ml);
            return "";
        }
    }

    // Get default sink info to find its monitor source
    struct SinkData { std::string monitor; };
    SinkData sinkData;

    pa_operation* op = pa_context_get_server_info(ctx,
        [](pa_context* c, const pa_server_info* info, void* userdata) {
            if (!info) return;
            auto* sd = static_cast<SinkData*>(userdata);
            // The monitor source is typically "<sink_name>.monitor"
            if (info->default_sink_name) {
                sd->monitor = std::string(info->default_sink_name) + ".monitor";
            }
        }, &sinkData);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml, 1, nullptr);
    }
    pa_operation_unref(op);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    return sinkData.monitor;
}

void AudioCapture::captureThread() {
    std::string deviceName = pulseDeviceName_;
    if (deviceName.empty()) {
        deviceName = getDefaultMonitorSource();
        if (deviceName.empty()) {
            printf("Could not find default monitor source\n");
            running_ = false;
            return;
        }
    }

    printf("Capturing from: %s\n", deviceName.c_str());

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = 48000;
    ss.channels = 2;

    pa_buffer_attr bufattr;
    memset(&bufattr, 0, sizeof(bufattr));
    bufattr.maxlength = (uint32_t)-1;
    bufattr.fragsize = 480 * 2 * sizeof(float); // 10ms at 48kHz stereo

    int error = 0;
    pa_simple* s = pa_simple_new(
        nullptr,                // default server
        "AudioBridge",          // app name
        PA_STREAM_RECORD,       // direction
        deviceName.c_str(),     // device
        "Audio Capture",        // stream name
        &ss,                    // sample spec
        nullptr,                // channel map
        &bufattr,               // buffer attributes
        &error
    );

    if (!s) {
        printf("pa_simple_new() failed: %s\n", pa_strerror(error));
        running_ = false;
        return;
    }

    const int frameSize = 480; // 10ms at 48kHz
    float buf[frameSize * 2]; // stereo

    while (running_) {
        if (pa_simple_read(s, buf, sizeof(buf), &error) < 0) {
            printf("pa_simple_read() failed: %s\n", pa_strerror(error));
            break;
        }

        if (callback_) {
            callback_(buf, frameSize);
        }
    }

    pa_simple_free(s);
}
