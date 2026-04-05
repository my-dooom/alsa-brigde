#include "audio_config.hpp"

#include <iostream>

int configure_device(
    snd_pcm_t* handle,
    const char* role,
    snd_pcm_format_t stream_format,
    unsigned sample_rate,
    unsigned channels,
    snd_pcm_uframes_t period_frames,
    unsigned buffer_periods
) {
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);

    int dir = 0;
    int err = snd_pcm_hw_params_any(handle, hw_params);
    if (err < 0) {
        std::cerr << role << " hw_params_any failed: " << snd_strerror(err) << "\n";
        return err;
    }

    err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED);
    if (err < 0) {
        std::cerr << role << " set_access failed: " << snd_strerror(err) << "\n";
        return err;
    }

    err = snd_pcm_hw_params_set_format(handle, hw_params, stream_format);
    if (err < 0) {
        std::cerr << role << " set_format(" << snd_pcm_format_name(stream_format)
                  << ") failed: " << snd_strerror(err) << "\n";
        return err;
    }

    // Enable ALSA resampling support when available.
    unsigned int resample = 1;
    snd_pcm_hw_params_set_rate_resample(handle, hw_params, resample);

    unsigned int rate = sample_rate;
    err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, &dir);
    if (err < 0) {
        std::cerr << role << " set_rate_near failed: " << snd_strerror(err) << "\n";
        return err;
    }

    snd_pcm_uframes_t frames = period_frames;
    err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &frames, &dir);
    if (err < 0) {
        std::cerr << role << " set_period_size_near failed: " << snd_strerror(err) << "\n";
        return err;
    }

    snd_pcm_uframes_t buffer_frames = period_frames * buffer_periods;
    err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_frames);
    if (err < 0) {
        std::cerr << role << " set_buffer_size_near failed: " << snd_strerror(err) << "\n";
        return err;
    }

    err = snd_pcm_hw_params_set_channels(handle, hw_params, channels);
    if (err < 0) {
        std::cerr << role << " set_channels failed: " << snd_strerror(err) << "\n";
        return err;
    }

    err = snd_pcm_hw_params(handle, hw_params);
    if (err < 0) {
        std::cerr << role << " hw_params apply failed: " << snd_strerror(err) << "\n";
        return err;
    }

    return 0;
}
