#pragma once

// Caller supplies synchronization. No allocation, floating point, or CRT calls:
// the same queue runs in the capture DPC and in the host-side tests.
template <unsigned Capacity>
class MicRing {
public:
    void clear() { read_ = 0; size_ = 0; }
    unsigned size() const { return size_; }
    void push(const short* samples, unsigned count) {
        if (count > Capacity) { samples += count - Capacity; count = Capacity; }
        if (size_ + count > Capacity) {
            const unsigned discard = size_ + count - Capacity;
            read_ = (read_ + discard) % Capacity;
            size_ -= discard;
        }
        for (unsigned i = 0; i < count; ++i)
            samples_[(read_ + size_ + i) % Capacity] = samples[i];
        size_ += count;
    }
    short pop() {
        if (!size_) return 0;
        const short sample = samples_[read_];
        read_ = (read_ + 1) % Capacity;
        --size_;
        return sample;
    }
private:
    short samples_[Capacity]{};
    unsigned read_ = 0;
    unsigned size_ = 0;
};

// A small prefill absorbs packet-arrival jitter without delaying each packet.
// The capture clock calls pop() once per 48 kHz sample, including during silence.
// A partial tail expires after 100 ms instead of leaking into a later mic session.
class MicPlaybackQueue {
public:
    void clear() { ring_.clear(); primed_ = false; waitingFrames_ = 0; }
    unsigned size() const { return ring_.size(); }
    void push(const short* samples, unsigned count) {
        if (count) { ring_.push(samples, count); waitingFrames_ = 0; }
    }
    short pop() {
        if (!primed_) {
            if (ring_.size() >= 576) primed_ = true; // 12 ms at 48 kHz.
            else {
                if (++waitingFrames_ >= 4800) { ring_.clear(); waitingFrames_ = 0; }
                return 0;
            }
        }
        if (!ring_.size()) { primed_ = false; waitingFrames_ = 0; return 0; }
        return ring_.pop();
    }
private:
    MicRing<1920> ring_;
    bool primed_ = false;
    unsigned waitingFrames_ = 0;
};
