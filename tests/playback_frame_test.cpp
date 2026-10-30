#include "playback_frame.h"
#ifdef _WIN32
#include "audiobridge_tap.h"
static_assert(sizeof(AudioBridgeTapSharedBuffer) == 64, "AudioBridgeTapSharedBuffer must be 64-byte cache-line aligned");
#endif
#include <cassert>
#include <cstring>
#include <vector>
#include <iostream>

// Verify sample order and bounds across packet/callback boundaries and live changes.
static void check(const std::vector<int>& packets, int callbackSize) {
    PlaybackFrame pending;
    size_t next = 0;
    int produced = 0, consumed = 0, total = 0;
    for (int size : packets) total += size;
    while (consumed < total) {
        const int requested = std::min(callbackSize, total - consumed);
        std::vector<int16_t> output(requested * 2 + 2, -12345);
        int written = 0;
        while (written < requested) {
            if (pending.empty()) {
                pending.reset();
                pending.frame.frameCount = packets.at(next++);
                for (int i = 0; i < pending.frame.frameCount * 2; ++i)
                    pending.frame.samples[i] = static_cast<int16_t>((produced * 2 + i) % 30000);
                produced += pending.frame.frameCount;
            }
            const int beforeRem = pending.remaining();
            const int count = pending.copyTo(output.data() + 1 + written * 2, requested - written);
            assert(count > 0);
            assert(pending.remaining() == beforeRem - count);
            written += count;
        }
        for (int i = 0; i < requested * 2; ++i)
            assert(output[i + 1] == (consumed * 2 + i) % 30000);
        assert(output.front() == -12345 && output.back() == -12345);
        consumed += requested;
    }
    assert(pending.empty() && pending.remaining() == 0);
    pending.frame.frameCount = 960;
    pending.reset();
    assert(pending.empty() && pending.remaining() == 0);
    int16_t guard = 123;
    assert(pending.copyTo(&guard, 1) == 0 && guard == 123);
}

// Verify muted-stream multi-packet draining (output + drained * 2) never overruns callback buffer.
static void checkMutedDrain() {
    PlaybackFrame pending;
    const int callbackFrames = 480;
    std::vector<int16_t> guarded(callbackFrames * 2 + 2, -23456);
    int16_t* output = guarded.data() + 1;

    std::vector<AudioFrame> queue(5);
    for (size_t p = 0; p < queue.size(); ++p) {
        queue[p].frameCount = 120; // 2.5 ms Raw PCM
        for (int i = 0; i < 120 * 2; ++i) queue[p].samples[i] = static_cast<int16_t>(1000 + p * 100 + i);
    }
    size_t qIdx = 0;
    int drained = 0;
    while (drained < callbackFrames) {
        if (pending.empty()) {
            pending.reset();
            if (qIdx >= queue.size()) break;
            pending.frame = queue[qIdx++];
        }
        drained += pending.copyTo(output + drained * 2, callbackFrames - drained);
    }
    assert(drained == 480);
    assert(qIdx == 4 && pending.empty());
    std::memset(output, 0, callbackFrames * 2 * sizeof(int16_t));
    assert(guarded.front() == -23456 && guarded.back() == -23456);
    for (int i = 0; i < callbackFrames * 2; ++i) assert(output[i] == 0);
}

#ifdef _WIN32
// Verify WaveRT kernel tap shared-memory ring wraparound, driver restart resync, and overrun jump.
static void checkKernelTapRingLogic() {
    const uint32_t capFrames = 256;
    const uint32_t channels = 2;
    const uint32_t quantum = 128;
    std::vector<unsigned char> mem(sizeof(AudioBridgeTapSharedBuffer) + capFrames * channels * sizeof(float), 0);
    auto* hdr = reinterpret_cast<AudioBridgeTapSharedBuffer*>(mem.data());
    hdr->magic = AUDIOBRIDGE_TAP_MAGIC;
    hdr->version = AUDIOBRIDGE_TAP_VERSION;
    hdr->sampleRate = AUDIOBRIDGE_TAP_SAMPLE_RATE;
    hdr->channels = channels;
    hdr->capacityFrames = capFrames;
    hdr->quantumFrames = quantum;
    float* ring = AudioBridgeTapGetSamples(hdr);
    const float* ringConst = AudioBridgeTapGetSamplesConst(hdr);
    assert(ring == ringConst);

    // 1. Cyclic wraparound across capFrames boundary (frame 200..327 mod 256)
    LONG64 readCursor = 200;
    hdr->writeFrameCursor = 200;
    for (uint32_t f = 0; f < quantum; ++f) {
        uint32_t idx = ((200 + f) % capFrames) * channels;
        ring[idx] = static_cast<float>(f);
        ring[idx + 1] = static_cast<float>(-static_cast<int>(f));
    }
    hdr->writeFrameCursor = 328;

    LONG64 writeCursor = hdr->writeFrameCursor;
    LONG64 avail = writeCursor - readCursor;
    assert(avail == 128);
    for (uint32_t f = 0; f < static_cast<uint32_t>(avail); ++f) {
        uint32_t idx = static_cast<uint32_t>((readCursor + f) % capFrames) * channels;
        assert(ringConst[idx] == static_cast<float>(f));
        assert(ringConst[idx + 1] == static_cast<float>(-static_cast<int>(f)));
    }
    readCursor += avail;
    assert(readCursor == 328);

    // 2. Reader overrun (avail > capFrames) -> jumps cleanly to latest quantum
    hdr->writeFrameCursor = 1000;
    writeCursor = hdr->writeFrameCursor;
    if (writeCursor - readCursor > static_cast<LONG64>(capFrames)) {
        const uint32_t q = hdr->quantumFrames;
        readCursor = writeCursor - ((q > 0 && q <= capFrames) ? q : AUDIOBRIDGE_TAP_DEFAULT_QUANTUM);
    }
    assert(readCursor == 1000 - 128);

    // 3. Driver stream restart (writeCursor < readCursor) -> resyncs instead of stalling
    hdr->writeFrameCursor = 0;
    writeCursor = hdr->writeFrameCursor;
    if (writeCursor < readCursor) {
        readCursor = writeCursor;
    }
    assert(readCursor == 0);
}
#endif

int main() {
    int combinations = 0;
    for (int callback : {48, 64, 96, 128, 144, 192, 240, 480, 512, 960}) {
        for (int packet : {120, 128, 240, 480, 960}) {
            check(std::vector<int>(16, packet), callback);
            ++combinations;
        }
        check({120, 128, 240, 960, 480, 120, 240, 480, 960, 128, 120}, callback);
        ++combinations;
    }
    checkMutedDrain();
#ifdef _WIN32
    checkKernelTapRingLogic();
#endif
    std::cout << "Playback & kernel-tap regression tests passed (" << combinations << " packet/callback combinations + muted drain + ring wraparound/resync).\n";
}
