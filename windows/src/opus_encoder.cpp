#include "opus_encoder.h"
#include <cstdio>

OpusEncoderWrapper::OpusEncoderWrapper() {}

OpusEncoderWrapper::~OpusEncoderWrapper() {
    if (encoder_) opus_encoder_destroy(encoder_);
}

bool OpusEncoderWrapper::initialize() {
    int error = 0;
    encoder_ = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    if (error != OPUS_OK || !encoder_) {
        printf("Failed to create Opus encoder: %s\n", opus_strerror(error));
        return false;
    }

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(8));

    printf("Opus encoder: 48kHz stereo, 128kbps, restricted low-delay, complexity 8\n");
    return true;
}

int OpusEncoderWrapper::encode(const float* pcm, uint32_t frameSize, uint8_t* output, int maxOutput) {
    int encoded = opus_encode_float(encoder_, pcm, frameSize, output, maxOutput);
    if (encoded < 0) {
        printf("Opus encode error: %s\n", opus_strerror(encoded));
        return 0;
    }
    return encoded;
}
