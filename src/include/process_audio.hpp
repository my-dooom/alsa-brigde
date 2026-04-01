#pragma once

#include <alsa/asoundlib.h>

bool process_block(
	snd_pcm_t* capture_handle,
	snd_pcm_t* playback_handle,
	float* buffer,
	snd_pcm_uframes_t frames,
	int& loop_count
);
