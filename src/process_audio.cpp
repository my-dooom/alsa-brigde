#include "process_audio.hpp"

#include <iostream>

void process_samples_inplace(
    float* buffer,
    snd_pcm_uframes_t frames,
    unsigned channels
) {
    (void)buffer;
    (void)frames;
    (void)channels;
}

bool process_block(
    snd_pcm_t* capture_handle,
    snd_pcm_t* playback_handle,
    float* buffer,
    snd_pcm_uframes_t frames,
    int& loop_count
) {
    int err = 0;

    // ---- read audio from capture device into buffer ----
    err = snd_pcm_readi(capture_handle,
                        &buffer[0], frames);
    if (err < 0)
    {
        // recover from xruns or other read errors
        std::cerr << "read error: " << snd_strerror(err) << ", preparing...\n";
        snd_pcm_prepare(capture_handle);
        return false;
    }

    process_samples_inplace(buffer, frames, 2U);

    // ---- write buffer to playback device ----
    err = snd_pcm_writei(playback_handle,
                            &buffer[0], frames);
    if (err < 0)
    {
        // recover from xruns or write errors
        std::cerr << "write error: " << snd_strerror(err) << ", preparing...\n";
        snd_pcm_prepare(playback_handle);
        return false;
    }

    loop_count++;
    if (loop_count % 1000 == 0) {
        std::cout << "Processed " << loop_count << " blocks\n";
    }

    return true;
}