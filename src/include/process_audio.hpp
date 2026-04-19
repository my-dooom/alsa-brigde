#pragma once

#include <alsa/asoundlib.h>
#include <cstddef>
#include <cstdint>

struct EffectParams {
	float reverb_decay;
	float reverb_mix;
	size_t reverb_delay_len;
};

void set_effect_target_params(const EffectParams& params);
EffectParams get_effect_target_params();
EffectParams get_effect_smoothed_params();

// Trance gate: bpm=0 disables the gate; depth 0.0=bypass 1.0=full.
void set_trance_gate_params(float bpm, float depth);

void process_samples_inplace(
	int32_t* buffer,
	snd_pcm_uframes_t frames,
	unsigned channels
);

void set_stream_format(snd_pcm_format_t format);

bool process_block(
	snd_pcm_t* capture_handle,
	snd_pcm_t* playback_handle,
	snd_pcm_uframes_t frames,
	int& loop_count
);
