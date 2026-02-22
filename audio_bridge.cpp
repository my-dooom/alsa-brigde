#include <alsa/asoundlib.h>   // alsa api for pcm audio capture/playback
#include <iostream>           // std::cout / std::cerr
#include <vector>             // std::vector for audio buffer
#include <cmath>              // math functions (not strictly needed here)

#define SAMPLE_RATE 48000     // audio sample rate in hz
#define CHANNELS 2            // stereo
#define FRAMES 256            // number of frames per read/write block

int main()
{
    snd_pcm_t *capture_handle;     // handle for input device (focusrite)
    snd_pcm_t *playback_handle;    // handle for output device (iqaudio)
    snd_pcm_hw_params_t *hw_params;// structure for hardware parameters

    int err; // used to store alsa return codes

    // ---- open capture device (focusrite) ----
    if ((err = snd_pcm_open(&capture_handle, "hw:1,0",
                            SND_PCM_STREAM_CAPTURE, 0)) < 0)
    {
        // print error if device fails to open
        std::cerr << "cannot open capture device: "
                  << snd_strerror(err) << std::endl;
        return 1; // exit if failure
    }

    // ---- open playback device (iqaudio dac+) ----
    if ((err = snd_pcm_open(&playback_handle, "hw:0,0",
                            SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
        // print error if device fails to open
        std::cerr << "cannot open playback device: "
                  << snd_strerror(err) << std::endl;
        return 1; // exit if failure
    }

    // ---- helper lambda to configure an alsa pcm device ----
    auto configure_device = [&](snd_pcm_t *handle)
    {
        snd_pcm_hw_params_alloca(&hw_params);                 // allocate hw params struct on stack
        snd_pcm_hw_params_any(handle, hw_params);             // initialize with default values
        snd_pcm_hw_params_set_access(handle, hw_params,
                                     SND_PCM_ACCESS_RW_INTERLEAVED); // interleaved lr lr lr...
        snd_pcm_hw_params_set_format(handle, hw_params,
                                     SND_PCM_FORMAT_FLOAT_LE); // 32-bit float little endian
        snd_pcm_hw_params_set_rate(handle, hw_params,
                                   SAMPLE_RATE, 0);           // set sample rate
        snd_pcm_hw_params_set_channels(handle, hw_params,
                                       CHANNELS);              // set number of channels
        snd_pcm_hw_params(handle, hw_params);                  // apply parameters to device
    };

    configure_device(capture_handle);   // configure input
    configure_device(playback_handle);  // configure output

    snd_pcm_prepare(capture_handle);    // prepare capture device for use
    snd_pcm_prepare(playback_handle);   // prepare playback device for use

    std::vector<float> buffer(FRAMES * CHANNELS); // audio buffer for one block

    std::cout << "running audio pass-through...\n";

    while (true) // main processing loop
    {
        // ---- read audio from focusrite into buffer ----
        err = snd_pcm_readi(capture_handle,
                            buffer.data(), FRAMES);
        if (err < 0)
        {
            // recover from xruns or other read errors
            snd_pcm_prepare(capture_handle);
            continue; // skip this cycle
        }

        // ---- simple transform: moving average  ----
        float last = 0.0f; // variable to hold last sample for moving average
        for (auto &sample : buffer)
        {
            sample = (sample + last) * 0.5f; // simple moving average
            last = sample;
        }

        // ---- write processed buffer to iqaudio ----
        err = snd_pcm_writei(playback_handle,
                             buffer.data(), FRAMES);
        if (err < 0)
        {
            // recover from xruns or write errors
            snd_pcm_prepare(playback_handle);
            continue; // skip this cycle
        }
    }

    snd_pcm_close(capture_handle);  // close input device
    snd_pcm_close(playback_handle); // close output device

    return 0; // exit program
}