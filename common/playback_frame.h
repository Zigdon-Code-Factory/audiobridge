#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

// Stereo PCM, with room for the largest supported packet (20 ms at 48 kHz).
struct AudioFrame {
    static constexpr int maxFrames = 960;
    int16_t samples[maxFrames * 2]{};
    int frameCount = 0;
    uint32_t sequence = 0;
    uint64_t timestamp = 0;
};

// Owned exclusively by the playback callback. Packet and callback sizes may differ.
class PlaybackFrame {
public:
    AudioFrame frame;
    void reset() { frame.frameCount = 0; offset_ = 0; }
    bool empty() const { return offset_ >= frame.frameCount; }
    int copyTo(int16_t* output, int requestedFrames) {
        const int count = std::min(requestedFrames, frame.frameCount - offset_);
        if (count <= 0) return 0;
        std::memcpy(output, frame.samples + offset_ * 2, count * 2 * sizeof(int16_t));
        offset_ += count;
        return count;
    }
private:
    int offset_ = 0;
};
