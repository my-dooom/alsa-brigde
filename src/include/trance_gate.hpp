#pragma once

#include <cstddef>
#include <cstdint>

// Rhythmic trance gate synced to an external BPM source.
//
// Divides each beat into 4 equal 16th-note subdivisions.  The first half of
// every subdivision is open (gain → 1.0); the second half is closed (gain → 0.0).
// Transitions are smoothed with a 2 ms linear ramp to avoid clicks.
//
// Usage (per audio period):
//   gate.set_bpm(128.0f);    // 0 or negative disables the gate
//   gate.set_depth(1.0f);    // 0.0 = bypass, 1.0 = full gate
//   gate.process_interleaved(buf, frames, channels);
namespace mydoom {

class TranceGate {
public:
    explicit TranceGate(unsigned sample_rate);

    // BPM from the Link session. 0 or negative → gate bypassed.
    void set_bpm(float bpm);

    // Mix depth: 0.0 = dry (bypass), 1.0 = full gate effect.
    void set_depth(float depth);

    // Apply gate in-place to an interleaved S32 stereo buffer.
    void process_interleaved(int32_t* buf, size_t frames, size_t channels);

private:
    unsigned sample_rate_;
    float    bpm_   = 0.0f;
    float    depth_ = 0.0f;

    // Absolute sample counter – used to track beat phase across periods.
    uint64_t phase_ = 0;

    // Per-frame smoothed gain (avoids zipper clicks at transitions).
    float gain_ = 1.0f;
};

} // namespace mydoom
