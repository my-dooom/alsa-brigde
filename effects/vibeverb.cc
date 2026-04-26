#include "vibeverb.hh"

#include <algorithm>
#include <cmath>

namespace {

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

// more comb filters in parallel to make the tail thicker
constexpr size_t kCombCount = 16;

// two diffusion passes to spread the input before the combs

// damping inside the tank so the combs dont stay too bright
constexpr float kTankDamping = 0.40f;

// small stereo bleed between channels for a wider sound
constexpr float kStereoCrossfeed = 0.20f;

} // namespace

namespace mydoom {

// default mix starts around 35% wet
Vibeverb::Vibeverb(float decay, size_t delay_len, size_t channels)
    : decay_(clampf(decay, 0.0f, 0.98f)),
      mix_(0.35f),
      diffusion_a_(0.72f),
      diffusion_b_(0.64f),
      base_delay_len_(delay_len > 0 ? delay_len : 1),
      channels_(channels > 0 ? channels : 1) {
    rebuild_state();
}

// keep the feedback below 1 so the reverb stays stable
void Vibeverb::set_decay(float decay) {
    decay_ = clampf(decay, 0.0f, 0.98f);
}

void Vibeverb::set_delay_length(size_t delay_len) {
    const size_t clamped = delay_len > 0 ? delay_len : 1;
    if (clamped == base_delay_len_) {
        return;
    }

    if (predelay_lines_.size() != channels_ ||
        predelay_index_.size() != channels_ ||
        diffuser_a_lines_.size() != channels_ ||
        diffuser_a_index_.size() != channels_ ||
        diffuser_b_lines_.size() != channels_ ||
        diffuser_b_index_.size() != channels_ ||
        comb_lines_.size() != channels_ ||
        comb_index_.size() != channels_ ||
        comb_damp_state_.size() != channels_ ||
        crossfeed_state_.size() != channels_) {
        base_delay_len_ = clamped;
        rebuild_state();
        return;
    }

    base_delay_len_ = clamped;

    // relative comb lengths so each parallel path is different
    constexpr float kCombScale[kCombCount] = {
        1.00f, 1.13f, 1.31f, 1.41f, 1.57f, 1.73f, 1.89f, 2.03f,
        2.11f, 2.23f, 2.37f, 2.51f, 2.63f, 2.77f, 2.89f, 3.13f
    };

    for (size_t ch = 0; ch < channels_; ++ch) {
        if (comb_lines_[ch].size() != kCombCount ||
            comb_index_[ch].size() != kCombCount ||
            comb_damp_state_[ch].size() != kCombCount) {
            rebuild_state();
            return;
        }

        // minimum base length and slight channel offset so the two channels differ
        const size_t base = std::max<size_t>(128, base_delay_len_ + ch * (base_delay_len_ / 9 + 29));

        // simple predelay before the diffusers
        const size_t predelay_len = std::max<size_t>(64, base / 3);
        predelay_lines_[ch].resize(predelay_len, 0.0f);
        predelay_index_[ch] %= predelay_lines_[ch].size();

        // two diffuser lengths chosen to avoid metallic resonance
        const size_t diff_a_len = std::max<size_t>(47, base / 7 + 31 + ch * 7);
        const size_t diff_b_len = std::max<size_t>(83, base / 5 + 59 + ch * 11);
        diffuser_a_lines_[ch].resize(diff_a_len, 0.0f);
        diffuser_b_lines_[ch].resize(diff_b_len, 0.0f);
        diffuser_a_index_[ch] %= diffuser_a_lines_[ch].size();
        diffuser_b_index_[ch] %= diffuser_b_lines_[ch].size();

        for (size_t i = 0; i < kCombCount; ++i) {
            // stagger comb lengths for a fuller tail
            // minimum 127 samples to avoid too short comb delay
            const size_t comb_len = std::max<size_t>(127,
                static_cast<size_t>(base * kCombScale[i]) + i * 41 + ch * 17);
            comb_lines_[ch][i].resize(comb_len, 0.0f);
            comb_index_[ch][i] %= comb_lines_[ch][i].size();
        }
    }
}

// keep wet mix within 0..1
void Vibeverb::set_mix(float wet_mix) {
    mix_ = clampf(wet_mix, 0.0f, 1.0f);
}

void Vibeverb::set_diffusion(float diffusion) {
    diffusion_a_ = clampf(diffusion, 0.0f, 0.99f);
}

void Vibeverb::set_diffusion_b(float diffusion) {
    diffusion_b_ = clampf(diffusion, 0.0f, 0.99f);
}

void Vibeverb::reset() {
    for (auto& line : predelay_lines_) {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    for (auto& line : diffuser_a_lines_) {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    for (auto& line : diffuser_b_lines_) {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    for (auto& channel_combs : comb_lines_) {
        for (auto& line : channel_combs) {
            std::fill(line.begin(), line.end(), 0.0f);
        }
    }

    std::fill(predelay_index_.begin(), predelay_index_.end(), 0);
    std::fill(diffuser_a_index_.begin(), diffuser_a_index_.end(), 0);
    std::fill(diffuser_b_index_.begin(), diffuser_b_index_.end(), 0);

    for (auto& idxs : comb_index_) {
        std::fill(idxs.begin(), idxs.end(), 0);
    }
    for (auto& states : comb_damp_state_) {
        std::fill(states.begin(), states.end(), 0.0f);
    }
    std::fill(crossfeed_state_.begin(), crossfeed_state_.end(), 0.0f);
}

float Vibeverb::process_allpass(std::vector<float>& line, size_t& index, float input, float coeff) {
    if (line.empty()) {
        return input;
    }

    // Classic all-pass filter structure used for diffusion: it preserves energy
    // while spreading transients in time, which helps avoid metallic comb ringing.
    const float delayed = line[index];
    const float output = delayed - coeff * input;
    line[index] = input + coeff * output;

    index = (index + 1) % line.size();
    return output;
}

void Vibeverb::process_interleaved(int32_t* buffer, size_t frames, size_t channels) {
    if (!buffer || frames == 0 || channels == 0) {
        return;
    }

    const size_t active_channels = std::min(channels_, channels);
    if (active_channels == 0) {
        return;
    }

    // scale 32-bit pcm to float and back
    constexpr float kScale = 2147483647.0f;
    constexpr float kInvScale = 1.0f / kScale;
    // average the comb outputs so the wet level stays stable
    const float comb_norm = 1.0f / static_cast<float>(kCombCount);

    std::vector<float> wet_values(active_channels, 0.0f);

    for (size_t frame = 0; frame < frames; ++frame) {
        for (size_t ch = 0; ch < active_channels; ++ch) {
            const size_t idx = frame * channels + ch;
            const float dry = static_cast<float>(buffer[idx]) * kInvScale;

            float x = dry;

            std::vector<float>& pre = predelay_lines_[ch];
            if (!pre.empty()) {
                float delayed = pre[predelay_index_[ch]];
                pre[predelay_index_[ch]] = x;
                predelay_index_[ch] = (predelay_index_[ch] + 1) % pre.size();
                x = delayed;
            }

            x = process_allpass(diffuser_a_lines_[ch], diffuser_a_index_[ch], x, diffusion_a_);
            x = process_allpass(diffuser_b_lines_[ch], diffuser_b_index_[ch], x, diffusion_b_);

            float comb_sum = 0.0f;
            for (size_t i = 0; i < kCombCount; ++i) {
                std::vector<float>& comb = comb_lines_[ch][i];
                if (comb.empty()) {
                    continue;
                }

                size_t& w = comb_index_[ch][i];
                const float delayed = comb[w];

                float& damp_state = comb_damp_state_[ch][i];
                damp_state = damp_state * kTankDamping + delayed * (1.0f - kTankDamping);

                const float feedback_in = x + damp_state * decay_;
                comb[w] = feedback_in;
                w = (w + 1) % comb.size();

                comb_sum += damp_state;
            }

            float wet = comb_sum * comb_norm;
            const size_t other = (ch + 1) % active_channels;
            // small crossfeed from other channel for stereo width
            wet = wet * (1.0f - kStereoCrossfeed) + crossfeed_state_[other] * kStereoCrossfeed;
            wet_values[ch] = wet;
        }

        for (size_t ch = 0; ch < active_channels; ++ch) {
            const size_t idx = frame * channels + ch;
            const float dry = static_cast<float>(buffer[idx]) * kInvScale;
            const float wet = wet_values[ch];

            crossfeed_state_[ch] = wet;

            // mix: dry always full, wet scaled by mix_
            const float out = dry + wet * mix_ * 3.0f; // extra gain on the wet signal to compensate for diffusion losses
            const float clipped = clampf(out, -1.0f, 1.0f);
            buffer[idx] = static_cast<int32_t>(clipped * kScale);
        }
    }
}

void Vibeverb::rebuild_state() {
    predelay_lines_.assign(channels_, {});
    predelay_index_.assign(channels_, 0);

    diffuser_a_lines_.assign(channels_, {});
    diffuser_a_index_.assign(channels_, 0);
    diffuser_b_lines_.assign(channels_, {});
    diffuser_b_index_.assign(channels_, 0);

    comb_lines_.assign(channels_, std::vector<std::vector<float>>(kCombCount));
    comb_index_.assign(channels_, std::vector<size_t>(kCombCount, 0));
    comb_damp_state_.assign(channels_, std::vector<float>(kCombCount, 0.0f));

    crossfeed_state_.assign(channels_, 0.0f);

    // relative comb lengths for the late tail
    constexpr float kCombScale[kCombCount] = {
        1.00f, 1.13f, 1.31f, 1.41f, 1.57f, 1.73f, 1.89f, 2.03f,
        2.11f, 2.23f, 2.37f, 2.51f, 2.63f, 2.77f, 2.89f, 3.13f
    };

    for (size_t ch = 0; ch < channels_; ++ch) {
        // minimum base length and per-channel offset
        const size_t base = std::max<size_t>(128, base_delay_len_ + ch * (base_delay_len_ / 9 + 29));

        // minimum predelay length for early spacing
        const size_t predelay_len = std::max<size_t>(64, base / 3);
        predelay_lines_[ch].assign(predelay_len, 0.0f);

        // diffuser lengths chosen to reduce metallic ringing
        const size_t diff_a_len = std::max<size_t>(47, base / 7 + 31 + ch * 7);
        const size_t diff_b_len = std::max<size_t>(83, base / 5 + 59 + ch * 11);
        diffuser_a_lines_[ch].assign(diff_a_len, 0.0f);
        diffuser_b_lines_[ch].assign(diff_b_len, 0.0f);

        for (size_t i = 0; i < kCombCount; ++i) {
            // minimum comb delay length and stagger offsets
            const size_t comb_len = std::max<size_t>(127,
                static_cast<size_t>(base * kCombScale[i]) + i * 41 + ch * 17);
            comb_lines_[ch][i].assign(comb_len, 0.0f);
        }
    }
}

} // namespace mydoom
