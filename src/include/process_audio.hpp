#pragma once

#include <alsa/asoundlib.h>
#include <cstdint>

struct EffectParams {
	float output_gain;    // linear gain applied after reverb [0, 2]
	float wet_mix;        // reverb wet/dry [0, 1]
	float reverb_decay;   // reverb tail to -60 dB, seconds [0.1, 10]
	float reverb_damping; // feedback LP coeff: 0 = bright, 1 = dark [0, 1)
	float reverb_bandwidth; // input bandwidth filter center freq [0, 1]
};

void set_effect_target_params(const EffectParams& params);
EffectParams get_effect_target_params();
EffectParams get_effect_smoothed_params();

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
