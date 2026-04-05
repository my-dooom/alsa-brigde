#pragma once

#include <alsa/asoundlib.h>

// Processes one mmap-based capture->playback block and advances loop_count on success.
bool process_block_mmap_impl(
    snd_pcm_t* capture_handle,
    snd_pcm_t* playback_handle,
    snd_pcm_uframes_t frames,
    int& loop_count
);
