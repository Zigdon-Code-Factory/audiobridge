#include "opus_encoder.h"
#include <cstdio>
#include <chrono>

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
    static auto lastEncodeTime = std::chrono::steady_clock::now();
    static int encodeCount = 0;
    static uint64_t slowEncodeCount = 0;
    static uint64_t largeGapCount = 0;
    static double maxEncodeMs = 0.0;
    static double maxGapMs = 0.0;

    auto beforeEncode = std::chrono::steady_clock::now();
    double gapMs = std::chrono::duration<double, std::milli>(beforeEncode - lastEncodeTime).count();

    int encoded = opus_encode_float(encoder_, pcm, frameSize, output, maxOutput);

    auto afterEncode = std::chrono::steady_clock::now();
    double encodeMs = std::chrono::duration<double, std::milli>(afterEncode - beforeEncode).count();
    lastEncodeTime = beforeEncode;

    if (encoded < 0) {
        printf("[OPUS-ENC] Encode error: %s (frameSize=%u)\n", opus_strerror(encoded), frameSize);
        return 0;
    }

    encodeCount++;

    if (encodeMs > maxEncodeMs) maxEncodeMs = encodeMs;
    if (gapMs > maxGapMs) maxGapMs = gapMs;

    // Flag slow encodes (> 2ms is too long for a 10ms frame)
    if (encodeMs > 2.0) {
        slowEncodeCount++;
        if (slowEncodeCount <= 20 || (slowEncodeCount % 100) == 0) {
            printf("[OPUS-ENC] SLOW encode #%llu: %.2fms (frameSize=%u, encoded=%d bytes)\n",
                   slowEncodeCount, encodeMs, frameSize, encoded);
        }
    }

    // Flag large inter-encode gaps (> 15ms means we're not keeping up with 10ms frames)
    if (gapMs > 15.0 && encodeCount > 1) {
        largeGapCount++;
        if (largeGapCount <= 20 || (largeGapCount % 100) == 0) {
            printf("[OPUS-ENC] LARGE GAP #%llu: %.1fms between encodes (expect pop)\n",
                   largeGapCount, gapMs);
        }
    }

    // Periodic summary
    if (encodeCount <= 5 || (encodeCount % 500) == 0) {
        float maxPcm = 0.0f, minPcm = 0.0f;
        for (uint32_t i = 0; i < frameSize * 2; i++) {
            if (pcm[i] > maxPcm) maxPcm = pcm[i];
            if (pcm[i] < minPcm) minPcm = pcm[i];
        }
        printf("[OPUS-ENC] #%d: frameSize=%u, encoded=%d bytes, PCM=[%.4f, %.4f], "
               "encTime=%.2fms, gap=%.1fms, maxEnc=%.2fms, maxGap=%.1fms, "
               "slowEnc=%llu, largeGaps=%llu\n",
               encodeCount, frameSize, encoded, minPcm, maxPcm,
               encodeMs, gapMs, maxEncodeMs, maxGapMs,
               slowEncodeCount, largeGapCount);
        // Reset per-report maxes
        maxEncodeMs = 0.0;
        maxGapMs = 0.0;
    }
    return encoded;
}
