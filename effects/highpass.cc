#include "highpass.hh"

#include <algorithm>
#include <cmath>

namespace {

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

// Logarithmic mix → cutoff mapping.
// mix=0 → 20 Hz, mix=1 → 10 000 Hz.
// Formula: fc = 20 * 500^mix  (20 * 500^0 = 20, 20 * 500^1 = 10 000)
float mix_to_cutoff_hz(float mix) {
    return 20.0f * std::pow(500.0f, mix);
}

} // namespace

namespace mydoom {

HighPassFilter::HighPassFilter(float sample_rate, size_t channels)
    : sample_rate_(sample_rate > 0.0f ? sample_rate : 48000.0f),
      channels_(channels > 0 ? channels : 1),
      b0_(1.0f), b1_(0.0f), b2_(0.0f),
      a1_(0.0f), a2_(0.0f),
      state_(channels_) {
    // initialise with mix=0 (20 Hz cutoff)
    update_coefficients(20.0f);
}

void HighPassFilter::set_mix(float mix) {
    const float clamped = clampf(mix, 0.0f, 1.0f);
    update_coefficients(mix_to_cutoff_hz(clamped));
}

void HighPassFilter::reset() {
    for (auto& s : state_) {
        s = BiquadState{};
    }
}

// RBJ high-pass biquad (Q = 0.7071 = Butterworth)
// w0 = 2π·fc/fs
// alpha = sin(w0) / (2·Q)
// b0 = (1 + cos(w0)) / 2
// b1 = -(1 + cos(w0))
// b2 = (1 + cos(w0)) / 2
// a0 = 1 + alpha  (used only for normalisation)
// a1 = -2·cos(w0)
// a2 = 1 - alpha
void HighPassFilter::update_coefficients(float cutoff_hz) {
    // clamp so w0 never hits 0 or π (degenerate poles)
    const float fc = clampf(cutoff_hz, 1.0f, sample_rate_ * 0.499f);
    const float w0    = 2.0f * static_cast<float>(M_PI) * fc / sample_rate_;
    const float cos_w = std::cos(w0);
    const float sin_w = std::sin(w0);

    constexpr float kQ = 2.5f; // resonant peak at cutoff
    const float alpha  = sin_w / (2.0f * kQ);
    const float a0_inv = 1.0f / (1.0f + alpha);

    b0_ =  (1.0f + cos_w) * 0.5f  * a0_inv;
    b1_ = -(1.0f + cos_w)         * a0_inv;
    b2_ =  (1.0f + cos_w) * 0.5f  * a0_inv;
    a1_ = (-2.0f * cos_w)         * a0_inv;
    a2_ =  (1.0f - alpha)         * a0_inv;
}

float HighPassFilter::process_sample(float x, size_t ch) {
    if (ch >= state_.size()) {
        return x;
    }
    BiquadState& s = state_[ch];
    const float y = b0_ * x + b1_ * s.x1 + b2_ * s.x2
                  - a1_ * s.y1 - a2_ * s.y2;
    s.x2 = s.x1;  s.x1 = x;
    s.y2 = s.y1;  s.y1 = y;
    return y;
}

void HighPassFilter::process_interleaved(int32_t* buffer, size_t frames, size_t channels) {
    if (!buffer || frames == 0 || channels == 0) {
        return;
    }

    const size_t active = std::min(channels_, channels);

    constexpr float kScale    = 2147483647.0f;
    constexpr float kInvScale = 1.0f / kScale;

    for (size_t frame = 0; frame < frames; ++frame) {
        for (size_t ch = 0; ch < active; ++ch) {
            const size_t i = frame * channels + ch;
            const float x = static_cast<float>(buffer[i]) * kInvScale;

            BiquadState& s = state_[ch];

            // direct-form I: y = b0·x + b1·x1 + b2·x2 - a1·y1 - a2·y2
            const float y = b0_ * x + b1_ * s.x1 + b2_ * s.x2
                          - a1_ * s.y1 - a2_ * s.y2;

            s.x2 = s.x1;
            s.x1 = x;
            s.y2 = s.y1;
            s.y1 = y;

            // clamp before converting back to avoid int32 overflow
            const float out = clampf(y, -1.0f, 1.0f);
            buffer[i] = static_cast<int32_t>(out * kScale);
        }
    }
}

} // namespace mydoom
