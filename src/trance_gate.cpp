#include "trance_gate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mydoom {

// One cycle per beat: open on downbeat, close on offbeat.
static constexpr unsigned kSubdivisions = 2;

TranceGate::TranceGate(unsigned sample_rate)
    : sample_rate_(sample_rate)
{}

void TranceGate::set_bpm(float bpm)
{
    bpm_ = bpm;
}

void TranceGate::set_depth(float depth)
{
    depth_ = std::max(0.0f, std::min(depth, 1.0f));
}

void TranceGate::process_interleaved(int32_t* buf, size_t frames, size_t channels)
{
    if (depth_ <= 0.0f || bpm_ <= 0.0f || frames == 0 || channels == 0) {
        // Gate bypassed – still advance phase so it stays coherent if re-enabled.
        if (bpm_ > 0.0f) {
            const auto beat_len = static_cast<uint64_t>(
                static_cast<float>(sample_rate_) * 60.0f / bpm_ + 0.5f);
            if (beat_len > 0) {
                phase_ = (phase_ + frames) % beat_len;
            }
        }
        return;
    }

    // Samples per beat and per 16th-note subdivision.
    const auto beat_len   = static_cast<uint64_t>(
        static_cast<float>(sample_rate_) * 60.0f / bpm_ + 0.5f);
    if (beat_len == 0) return;

    const uint64_t subdiv_len = beat_len / kSubdivisions;
    if (subdiv_len == 0) return;

    // 2 ms linear ramp rate (max gain change per sample).
    const float ramp_rate = 1.0f / (0.002f * static_cast<float>(sample_rate_));

    for (size_t f = 0; f < frames; ++f) {
        const uint64_t subdiv_phase = phase_ % subdiv_len;
        const float target = (subdiv_phase < subdiv_len / 2) ? 0.0f : 1.0f;

        // Advance gain_ toward target at ramp_rate (linear, click-free).
        const float diff = target - gain_;
        if (diff > ramp_rate)       gain_ += ramp_rate;
        else if (diff < -ramp_rate) gain_ -= ramp_rate;
        else                        gain_  = target;

        // Blend: at depth=1 → full gate; at depth=0 → dry.
        const float applied = gain_ * depth_ + (1.0f - depth_);

        for (size_t ch = 0; ch < channels; ++ch) {
            const size_t idx = f * channels + ch;
            buf[idx] = static_cast<int32_t>(
                static_cast<float>(buf[idx]) * applied);
        }

        phase_ = (phase_ + 1) % beat_len;
    }
}

} // namespace mydoom
