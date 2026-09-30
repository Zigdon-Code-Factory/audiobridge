#include "playback_frame.h"
#include <cassert>
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
            const int count = pending.copyTo(output.data() + 1 + written * 2, requested - written);
            assert(count > 0);
            written += count;
        }
        for (int i = 0; i < requested * 2; ++i)
            assert(output[i + 1] == (consumed * 2 + i) % 30000);
        assert(output.front() == -12345 && output.back() == -12345);
        consumed += requested;
    }
    assert(pending.empty());
    pending.frame.frameCount = 960;
    pending.reset();
    assert(pending.empty());
    int16_t guard = 123;
    assert(pending.copyTo(&guard, 1) == 0 && guard == 123);
}

int main() {
    for (int callback : {192, 240, 480, 512, 960}) {
        for (int packet : {240, 480, 960}) check(std::vector<int>(12, packet), callback);
        check({240, 960, 480, 240, 480, 960, 240}, callback);
    }
    std::cout << "Playback regression tests passed (20 packet/callback combinations).\n";
}
