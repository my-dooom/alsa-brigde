#include "process_audio.hpp"

#include "process_block_mmap.hpp"
#include "reverb.hpp"
#include "stream_format.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace {

std::atomic<float> g_target_gain{1.0f};
std::atomic<float> g_target_wet_mix{1.0f};
std::atomic<float> g_target_reverb_decay{2.0f};
std::atomic<float> g_target_reverb_damping{0.9f};
std::atomic<float> g_target_reverb_bandwidth{0.1f};
std::atomic<float> g_smoothed_gain_snapshot{1.0f};
std::atomic<float> g_smoothed_wet_snapshot{1.0f};

// Fixed at hardware sample rate; matches SAMPLE_RATE in audio_bridge.cpp.
Reverb g_reverb{48000.0, 2.0f, 1.0f};

// Max interleaved samples per process_samples_inplace call (512 frames * 2 ch).
constexpr size_t kMaxFloatBufSamples = 4096;

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

} // namespace

void set_effect_target_params(const EffectParams& params) {
    g_target_gain.store(clampf(params.output_gain, 0.0f, 2.0f), std::memory_order_relaxed);
    g_target_wet_mix.store(clampf(params.wet_mix, 0.0f, 1.0f), std::memory_order_relaxed);
    g_target_reverb_decay.store(std::max(0.1f, params.reverb_decay), std::memory_order_relaxed);
    g_target_reverb_damping.store(std::clamp(params.reverb_damping, 0.0f, 0.9999f), std::memory_order_relaxed);
    g_target_reverb_bandwidth.store(std::clamp(params.reverb_bandwidth, 0.0f, 1.0f), std::memory_order_relaxed);
}

EffectParams get_effect_target_params() {
    return EffectParams{
        g_target_gain.load(std::memory_order_relaxed),
        g_target_wet_mix.load(std::memory_order_relaxed),
        g_target_reverb_decay.load(std::memory_order_relaxed),
        g_target_reverb_damping.load(std::memory_order_relaxed),
        g_target_reverb_bandwidth.load(std::memory_order_relaxed)
    };
}

EffectParams get_effect_smoothed_params() {
    return EffectParams{
        g_smoothed_gain_snapshot.load(std::memory_order_relaxed),
        g_smoothed_wet_snapshot.load(std::memory_order_relaxed),
        g_target_reverb_decay.load(std::memory_order_relaxed),
        g_target_reverb_damping.load(std::memory_order_relaxed),
        g_target_reverb_bandwidth.load(std::memory_order_relaxed)
    };
}

void set_stream_format(snd_pcm_format_t format) {
    set_stream_format_internal(format);
}

void process_samples_inplace(int32_t* buffer_ptr, snd_pcm_uframes_t frames, unsigned channels) {
    if (!buffer_ptr || channels == 0 || frames == 0) {
        return;
    }

    const size_t total_samples = static_cast<size_t>(frames) * channels;
    if (total_samples > kMaxFloatBufSamples) {
        return;
    }

    constexpr float kSmoothingFactor = 0.0025f;
    constexpr float kScale           = 1.0f / 2147483648.0f;
    constexpr float kInt32Max        = 2147483647.0f;

    static float smoothed_gain = 1.0f;
    static float smoothed_wet  = 1.0f;
    static float float_buf[kMaxFloatBufSamples];

    const float target_gain  = g_target_gain.load(std::memory_order_relaxed);
    const float target_wet   = g_target_wet_mix.load(std::memory_order_relaxed);
    const float target_decay   = g_target_reverb_decay.load(std::memory_order_relaxed);
    const float target_damping = g_target_reverb_damping.load(std::memory_order_relaxed);
    const float target_bandwidth = g_target_reverb_bandwidth.load(std::memory_order_relaxed);

    // Smooth gain and wet over the block (per-block, not per-sample).
    smoothed_gain += (target_gain - smoothed_gain) * kSmoothingFactor * static_cast<float>(frames);
    smoothed_wet  += (target_wet  - smoothed_wet)  * kSmoothingFactor * static_cast<float>(frames);

    // int32 → float normalised [-1, 1]
    for (size_t i = 0; i < total_samples; ++i) {
        float_buf[i] = static_cast<float>(buffer_ptr[i]) * kScale;
    }

    // Update reverb params and run.
    g_reverb.set_decay_time(target_decay);
    g_reverb.set_wet_mix(smoothed_wet);
    g_reverb.set_damping(target_damping);
    g_reverb.set_bandwidth(target_bandwidth);
    g_reverb.process_samples_inplace(float_buf, frames, channels);

    // Apply output gain and convert float → int32.
    for (size_t i = 0; i < total_samples; ++i) {
        const float out = clampf(float_buf[i] * smoothed_gain, -1.0f, 1.0f);
        buffer_ptr[i] = static_cast<int32_t>(out * kInt32Max);
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
    return process_block_mmap_impl(capture_handle, playback_handle, frames, loop_count);
}

