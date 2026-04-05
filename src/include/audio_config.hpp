#pragma once

#include <alsa/asoundlib.h>

// Configures one ALSA PCM endpoint for mmap interleaved streaming.
// Returns 0 on success, or a negative ALSA error code on failure.
int configure_device(
    snd_pcm_t* handle,
    const char* role,
    snd_pcm_format_t stream_format,
    unsigned sample_rate,
    unsigned channels,
    snd_pcm_uframes_t period_frames,
    unsigned buffer_periods
);
