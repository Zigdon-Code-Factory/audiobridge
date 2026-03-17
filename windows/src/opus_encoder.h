#pragma once
#include <opus.h>
#include <cstdint>
#include <vector>

class OpusEncoderWrapper {
public:
    OpusEncoderWrapper();
    ~OpusEncoderWrapper();

    bool initialize();

    // Encode 480 interleaved float stereo samples → opus packet
    // Returns encoded size, or 0 on error
    int encode(const float* pcm, uint32_t frameSize, uint8_t* output, int maxOutput);

private:
    ::OpusEncoder* encoder_ = nullptr;
};
