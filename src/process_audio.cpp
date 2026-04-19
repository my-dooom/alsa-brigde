#include "process_audio.hpp"

#include "highpass.hh"
#include "process_block_mmap.hpp"
#include "stream_format.hpp"
#include "vibeverb.hh"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace {

std::atomic<float> g_target_decay{0.65f};
std::atomic<float> g_target_mix{0.35f};
std::atomic<uint32_t> g_target_delay_len{1728};
std::atomic<float> g_smoothed_decay_snapshot{0.65f};
std::atomic<float> g_smoothed_mix_snapshot{0.35f};
std::atomic<uint32_t> g_smoothed_delay_len_snapshot{1728};

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

} // namespace

void set_effect_target_params(const EffectParams& params) {
    g_target_decay.store(clampf(params.reverb_decay, 0.0f, 0.999f), std::memory_order_relaxed);
    g_target_mix.store(clampf(params.reverb_mix, 0.0f, 1.0f), std::memory_order_relaxed);
    const size_t clamped_delay_len = std::max<size_t>(1, std::min<size_t>(params.reverb_delay_len, 48000));
    g_target_delay_len.store(static_cast<uint32_t>(clamped_delay_len), std::memory_order_relaxed);
}

EffectParams get_effect_target_params() {
    return EffectParams{
        g_target_decay.load(std::memory_order_relaxed),
        g_target_mix.load(std::memory_order_relaxed),
        static_cast<size_t>(g_target_delay_len.load(std::memory_order_relaxed))
    };
}

EffectParams get_effect_smoothed_params() {
    return EffectParams{
        g_smoothed_decay_snapshot.load(std::memory_order_relaxed),
        g_smoothed_mix_snapshot.load(std::memory_order_relaxed),
        static_cast<size_t>(g_smoothed_delay_len_snapshot.load(std::memory_order_relaxed))
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

    constexpr float kSmoothingFactor = 0.012f;

    static float smoothed_decay = 0.65f;
    static float smoothed_mix = 0.35f;
    static uint32_t smoothed_delay_len = 1728;
    static mydoom::Vibeverb vibeverb(0.65f, 1728, 2);
    static mydoom::HighPassFilter hpf(48000.0f, 2);

    const float target_decay = g_target_decay.load(std::memory_order_relaxed);
    const float target_mix = g_target_mix.load(std::memory_order_relaxed);
    const uint32_t target_delay_len = g_target_delay_len.load(std::memory_order_relaxed);

    smoothed_decay += (target_decay - smoothed_decay) * kSmoothingFactor;
    smoothed_mix += (target_mix - smoothed_mix) * kSmoothingFactor;
    smoothed_delay_len = target_delay_len;

    vibeverb.set_decay(smoothed_decay);
    vibeverb.set_mix(smoothed_mix);
    vibeverb.set_delay_length(static_cast<size_t>(smoothed_delay_len));

    hpf.set_mix(smoothed_mix);
    hpf.process_interleaved(buffer_ptr, static_cast<size_t>(frames), static_cast<size_t>(channels));
    vibeverb.process_interleaved(buffer_ptr, static_cast<size_t>(frames), static_cast<size_t>(channels));

    g_smoothed_decay_snapshot.store(smoothed_decay, std::memory_order_relaxed);
    g_smoothed_mix_snapshot.store(smoothed_mix, std::memory_order_relaxed);
    g_smoothed_delay_len_snapshot.store(smoothed_delay_len, std::memory_order_relaxed);
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
