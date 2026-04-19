#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mydoom {

// Two-pole biquad high-pass filter.
// Cutoff maps logarithmically: mix=0 → 20 Hz, mix=1 → 10 kHz.
// Call set_mix() whenever the knob value changes; coefficients update in-place.
class HighPassFilter {
public:
    HighPassFilter(float sample_rate, size_t channels = 2);

    // mix in [0, 1]; 0 = 20 Hz, 1 = 10 kHz
    void set_mix(float mix);
    void reset();

    // Process one float sample for the given channel. Used when integrated
    // into another effect's float signal path.
    float process_sample(float x, size_t ch);

    void process_interleaved(int32_t* buffer, size_t frames, size_t channels);

private:
    void update_coefficients(float cutoff_hz);

    float sample_rate_;
    size_t channels_;

    // biquad direct-form I coefficients (normalised: a0 = 1)
    float b0_, b1_, b2_;
    float a1_, a2_;

    // per-channel delay state: [x(n-1), x(n-2), y(n-1), y(n-2)]
    struct BiquadState {
        float x1 = 0.0f, x2 = 0.0f;
        float y1 = 0.0f, y2 = 0.0f;
    };
    std::vector<BiquadState> state_;
};

} // namespace mydoom
