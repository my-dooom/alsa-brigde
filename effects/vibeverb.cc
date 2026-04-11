#include "vibeverb.hh"

#include <algorithm>
#include <cmath>

namespace {

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

size_t next_power_of_two(size_t value) {
    size_t p = 1;
    while (p < value) {
        p <<= 1;
    }
    return p;
}

} // namespace

namespace mydoom {

Vibeverb::Vibeverb(float decay, size_t delay_len, size_t channels)
    : decay_(clampf(decay, 0.0f, 0.999f)),
      mix_(0.35f),
      base_delay_len_(delay_len > 0 ? delay_len : 1),
      channels_(channels > 0 ? channels : 1) {
    rebuild_state();
}

void Vibeverb::set_decay(float decay) {
    decay_ = clampf(decay, 0.0f, 0.999f);
}

void Vibeverb::set_delay_length(size_t delay_len) {
    const size_t clamped = delay_len > 0 ? delay_len : 1;
    if (clamped == base_delay_len_) {
        return;
    }

    if (delay_lines_.size() != channels_ || write_index_.size() != channels_ || tap_offset_.size() != channels_) {
        base_delay_len_ = clamped;
        rebuild_state();
        return;
    }

    base_delay_len_ = clamped;

    // Retune delay lengths while preserving accumulated tail data.
    for (size_t ch = 0; ch < channels_; ++ch) {
        const size_t spread = (ch * (base_delay_len_ / 5 + 1)) + ch + 1;
        const size_t new_delay_len = base_delay_len_ + spread;

        std::vector<float>& line = delay_lines_[ch];
        line.resize(new_delay_len, 0.0f);
        if (line.empty()) {
            line.assign(1, 0.0f);
        }

        write_index_[ch] %= line.size();
        const size_t min_tap = std::min(line.size(), std::max<size_t>(32, line.size() / 4));
        if (min_tap >= line.size()) {
            tap_offset_[ch] = line.size();
        } else {
            const size_t variable_span = line.size() - min_tap;
            tap_offset_[ch] = min_tap + (spread % variable_span);
        }
    }
}

void Vibeverb::set_mix(float wet_mix) {
    mix_ = clampf(wet_mix, 0.0f, 1.0f);
}

void Vibeverb::reset() {
    for (auto& line : delay_lines_) {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    std::fill(write_index_.begin(), write_index_.end(), 0);
    std::fill(stage_.begin(), stage_.end(), 0.0f);
    std::fill(diffusion_state_.begin(), diffusion_state_.end(), 0.0f);
}

void Vibeverb::process_interleaved(int32_t* buffer, size_t frames, size_t channels) {
    if (!buffer || frames == 0 || channels == 0) {
        return;
    }

    const size_t active_channels = std::min(channels_, channels);
    if (active_channels == 0) {
        return;
    }

    constexpr float kScale = 2147483647.0f;
    constexpr float kInvScale = 1.0f / kScale;
    constexpr float kDiffusionSmoothing = 0.92f;
    constexpr float kCrossfeedNext = 0.28f;
    constexpr float kCrossfeedPrev = 0.28f;

    for (size_t frame = 0; frame < frames; ++frame) {
        for (size_t ch = 0; ch < active_channels; ++ch) {
            const size_t source_ch = shuffle_index_[ch];
            const std::vector<float>& source_line = delay_lines_[source_ch];
            const size_t source_write = write_index_[source_ch];
            const size_t source_size = source_line.size();
            const size_t source_read = (source_write + source_size - tap_offset_[source_ch]) % source_size;
            const float shuffled = source_line[source_read] * polarity_[ch];

            const size_t idx = frame * channels + ch;
            const float dry = static_cast<float>(buffer[idx]) * kInvScale;
            stage_[ch] = dry + shuffled * decay_;
        }

        std::copy(stage_.begin(), stage_.begin() + active_channels, hadamard_buffer_.begin());
        hadamard_in_place(hadamard_buffer_);
        const float normalizer = 1.0f / std::sqrt(static_cast<float>(next_power_of_two(active_channels)));

        for (size_t ch = 0; ch < active_channels; ++ch) {
            hadamard_buffer_[ch] *= normalizer;

            const size_t next_neighbor = (ch + 1) % active_channels;
            const size_t prev_neighbor = (ch + active_channels - 1) % active_channels;
            const float cross = hadamard_buffer_[next_neighbor] * kCrossfeedNext
                + hadamard_buffer_[prev_neighbor] * kCrossfeedPrev;
            const float diffused = hadamard_buffer_[ch] + cross;
            const float smoothed = diffusion_state_[ch] * kDiffusionSmoothing
                + diffused * (1.0f - kDiffusionSmoothing);
            diffusion_state_[ch] = smoothed;

            std::vector<float>& line = delay_lines_[ch];
            line[write_index_[ch]] = smoothed;
            write_index_[ch] = (write_index_[ch] + 1) % line.size();

            const size_t idx = frame * channels + ch;
            const float dry = static_cast<float>(buffer[idx]) * kInvScale;
            const float wet = smoothed;
            const float out = dry * (1.0f - mix_) + wet * mix_;
            const float clipped = clampf(out, -1.0f, 1.0f);
            buffer[idx] = static_cast<int32_t>(clipped * kScale);
        }
    }
}

void Vibeverb::rebuild_state() {
    delay_lines_.assign(channels_, {});
    write_index_.assign(channels_, 0);
    tap_offset_.assign(channels_, 1);
    shuffle_index_.assign(channels_, 0);
    polarity_.assign(channels_, 1.0f);
    stage_.assign(channels_, 0.0f);
    hadamard_buffer_.assign(channels_, 0.0f);
    diffusion_state_.assign(channels_, 0.0f);

    // Use staggered delays so channels decorrelate and create a wider tail.
    for (size_t ch = 0; ch < channels_; ++ch) {
        const size_t spread = (ch * (base_delay_len_ / 5 + 1)) + ch + 1;
        const size_t delay_len = base_delay_len_ + spread;
        delay_lines_[ch].assign(delay_len, 0.0f);
        const size_t min_tap = std::min(delay_len, std::max<size_t>(32, delay_len / 4));
        if (min_tap >= delay_len) {
            tap_offset_[ch] = delay_len;
        } else {
            const size_t variable_span = delay_len - min_tap;
            tap_offset_[ch] = min_tap + (spread % variable_span);
        }

        shuffle_index_[ch] = (ch * 3 + 1) % channels_;
        polarity_[ch] = (ch % 2 == 0) ? 1.0f : -1.0f;
    }
}

void Vibeverb::hadamard_in_place(std::vector<float>& values) {
    if (values.empty()) {
        return;
    }

    const size_t original_size = values.size();
    const size_t padded = next_power_of_two(original_size);
    values.resize(padded, 0.0f);

    for (size_t len = 1; len < padded; len <<= 1) {
        for (size_t i = 0; i < padded; i += (len << 1)) {
            for (size_t j = 0; j < len; ++j) {
                const float a = values[i + j];
                const float b = values[i + j + len];
                values[i + j] = a + b;
                values[i + j + len] = a - b;
            }
        }
    }

    values.resize(original_size);
}

} // namespace mydoom