#include "process_audio.hpp"

#include "process_block_mmap.hpp"
#include "stream_format.hpp"

// Public API shim: keep existing function names while delegating to split modules.
void set_stream_format(snd_pcm_format_t format) {
    // Persist negotiated stream format for use by block processing path.
    set_stream_format_internal(format);
}

void process_samples_inplace(int32_t* buffer_ptr, snd_pcm_uframes_t frames, unsigned channels) {
    // DSP hook is intentionally a no-op for now; kept for tests and future effects.
    (void)buffer_ptr;
    (void)frames;
    (void)channels;
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
