#include "process_audio.hpp"

#include "process_block_mmap.hpp"
#include "stream_format.hpp"

#include <algorithm>
#include <atomic>

namespace {

std::atomic<float> g_target_gain{1.0f};
std::atomic<float> g_target_wet_mix{1.0f};
std::atomic<float> g_smoothed_gain_snapshot{1.0f};
std::atomic<float> g_smoothed_wet_snapshot{1.0f};

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

} // namespace

void set_effect_target_params(const EffectParams& params) {
    g_target_gain.store(clampf(params.output_gain, 0.0f, 2.0f), std::memory_order_relaxed);
    g_target_wet_mix.store(clampf(params.wet_mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

EffectParams get_effect_target_params() {
    return EffectParams{
        g_target_gain.load(std::memory_order_relaxed),
        g_target_wet_mix.load(std::memory_order_relaxed)
    };
}

EffectParams get_effect_smoothed_params() {
    return EffectParams{
        g_smoothed_gain_snapshot.load(std::memory_order_relaxed),
        g_smoothed_wet_snapshot.load(std::memory_order_relaxed)
    };
}

// Public API shim: keep existing function names while delegating to split modules.
void set_stream_format(snd_pcm_format_t format) {
    // Persist negotiated stream format for use by block processing path.
    set_stream_format_internal(format);
}

void process_samples_inplace(int32_t* buffer_ptr, snd_pcm_uframes_t frames, unsigned channels) {
    if (!buffer_ptr || channels == 0 || frames == 0) {
        return;
    }

    constexpr float kSmoothingFactor = 0.0025f;
    constexpr float kInt32Min = -2147483648.0f;
    constexpr float kInt32Max = 2147483647.0f;

    static float smoothed_gain = 1.0f;
    static float smoothed_wet = 1.0f;

    const float target_gain = g_target_gain.load(std::memory_order_relaxed);
    const float target_wet = g_target_wet_mix.load(std::memory_order_relaxed);

    for (snd_pcm_uframes_t frame = 0; frame < frames; ++frame) {
        smoothed_gain += (target_gain - smoothed_gain) * kSmoothingFactor;
        smoothed_wet += (target_wet - smoothed_wet) * kSmoothingFactor;

        for (unsigned ch = 0; ch < channels; ++ch) {
            const size_t index = static_cast<size_t>(frame) * channels + ch;
            const float in = static_cast<float>(buffer_ptr[index]);
            const float effected = in * smoothed_gain;
            const float mixed = in + (effected - in) * smoothed_wet;
            const float clamped = clampf(mixed, kInt32Min, kInt32Max);
            buffer_ptr[index] = static_cast<int32_t>(clamped);
        }
    }

    g_smoothed_gain_snapshot.store(smoothed_gain, std::memory_order_relaxed);
    g_smoothed_wet_snapshot.store(smoothed_wet, std::memory_order_relaxed);
}

bool process_block(
    snd_pcm_t* capture_handle,
    snd_pcm_t* playback_handle,
    snd_pcm_uframes_t frames,
    int& loop_count
) {
    // Route all runtime block handling through the mmap implementation module.
    return process_block_mmap_impl(capture_handle, playback_handle, frames, loop_count);
}
