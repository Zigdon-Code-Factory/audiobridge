#include "opus_decoder.h"

OpusDecoderWrapper::OpusDecoderWrapper() {}

OpusDecoderWrapper::~OpusDecoderWrapper() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
}

bool OpusDecoderWrapper::initialize() {
    int error = 0;
    // Mono decoder — mic audio from phone is mono
    channels_ = 1;
    decoder_ = opus_decoder_create(48000, channels_, &error);
    return error == OPUS_OK && decoder_ != nullptr;
}

int OpusDecoderWrapper::decode(const uint8_t* opusData, int opusLen, float* pcmOutput, int maxFrames) {
    if (!decoder_) return 0;
    
    // Pass null for opusData if we want PLC (packet loss concealment)
    int decoded_frames = opus_decode_float(decoder_, opusData, opusLen, pcmOutput, maxFrames, 0);
    return decoded_frames > 0 ? decoded_frames : 0;
}
