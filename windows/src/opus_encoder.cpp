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
        printf("[OPUS-ENC] Encode error: %s (frameSize=%u)\n", opus_strerror(encoded), frameSize);
        return 0;
    }
    // Periodic logging to help diagnose audio quality issues
    static int encodeCount = 0;
    encodeCount++;
    if (encodeCount <= 5 || (encodeCount % 500) == 0) {
        // Log PCM input stats and opus output bytes
        float maxPcm = 0.0f, minPcm = 0.0f;
        for (uint32_t i = 0; i < frameSize * 2; i++) {
            if (pcm[i] > maxPcm) maxPcm = pcm[i];
            if (pcm[i] < minPcm) minPcm = pcm[i];
        }
        printf("[OPUS-ENC] #%d: frameSize=%u, encoded=%d bytes, PCM=[%.4f, %.4f], opus[0..3]=%02x %02x %02x %02x\n",
               encodeCount, frameSize, encoded, minPcm, maxPcm,
               encoded > 0 ? output[0] : 0,
               encoded > 1 ? output[1] : 0,
               encoded > 2 ? output[2] : 0,
               encoded > 3 ? output[3] : 0);
    }
    return encoded;
}
