#pragma once
#include <opus.h>
#include <cstdint>
#include <vector>

class OpusDecoderWrapper {
public:
    OpusDecoderWrapper();
    ~OpusDecoderWrapper();

    bool initialize();

    // Decode an opus packet to 48kHz mono float samples
    // Returns number of frames decoded (e.g., 480 for 10ms frame), or 0 on error
    int decode(const uint8_t* opusData, int opusLen, float* pcmOutput, int maxFrames);

    int getChannels() const { return channels_; }

private:
    ::OpusDecoder* decoder_ = nullptr;
    int channels_ = 1;
};
