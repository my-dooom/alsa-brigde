#pragma once

#include <alsa/asoundlib.h>
#include <cstdint>

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
