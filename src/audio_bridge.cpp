#include <alsa/asoundlib.h>   // alsa api for pcm audio capture/playback
#include <iostream>           // std::cout / std::cerr
#include <vector>             // std::vector for audio buffer
#include <string>             // std::string for device names
#include <utility>            // std::swap

#include "process_audio.hpp"

#define SAMPLE_RATE 48000     // audio sample rate in hz
#define CHANNELS 2            // stereo
#define FRAMES 128            // number of frames per read/write block

struct DeviceHandles
{
    snd_pcm_t* capture;
    snd_pcm_t* playback;
};

int main()
{

    std::string capture_device = "plughw:0,0";
    std::string playback_device = "plughw:1,0";

    DeviceHandles handles; 
    snd_pcm_hw_params_t *hw_params;// structure for hardware parameters

    int err; // used to store alsa return codes
    snd_pcm_uframes_t frames;
    unsigned int val;
    int dir;

    // ---- open capture device (focusrite) ----
    bool swapped = false;
    std::string original_capture = capture_device;
    if ((err = snd_pcm_open(&handles.capture, capture_device.c_str(),
                            SND_PCM_STREAM_CAPTURE, 0)) < 0)
    {
        // try swapping devices
        std::swap(capture_device, playback_device);
        swapped = true;
        std::cerr << "Failed to open capture on '" << original_capture << "', trying swapped devices...\n";
        if ((err = snd_pcm_open(&handles.capture, capture_device.c_str(),
                                SND_PCM_STREAM_CAPTURE, 0)) < 0)
        {
            std::cerr << "cannot open capture device '" << capture_device << "' even after swap: "
                      << snd_strerror(err) << std::endl;
            return 1;
        }
    }

    // ---- open playback device (iqaudio dac+) ----
    if ((err = snd_pcm_open(&handles.playback, playback_device.c_str(),
                            SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
        std::cerr << "cannot open playback device '" << playback_device << "': "
                  << snd_strerror(err) << std::endl;
        return 1; // exit if failure
    }

    if (swapped) {
        std::cout << "Devices were swapped: capture=" << capture_device << ", playback=" << playback_device << "\n";
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
          /* 48000 bits/second sampling rate */
        val = SAMPLE_RATE;
        snd_pcm_hw_params_set_rate_near(handle, hw_params,
                                        &val, &dir);

        /* Set period size to 256 frames. */
        frames = 256;
        snd_pcm_hw_params_set_period_size_near(handle, hw_params, &frames, &dir);
        snd_pcm_hw_params_set_channels(handle, hw_params,
                                       CHANNELS);              // set number of channels
        snd_pcm_hw_params(handle, hw_params);                  // apply parameters to device
    };

    configure_device(handles.capture);   // configure input
    configure_device(handles.playback);  // configure output

    snd_pcm_prepare(handles.capture);    // prepare capture device for use
    snd_pcm_prepare(handles.playback);   // prepare playback device for use

    /* Use a buffer large enough to hold one period */
    snd_pcm_hw_params_get_period_size(hw_params, &frames,
                                      &dir);
    std::vector<float> buffer(frames * CHANNELS); // audio buffer for one block (FLOAT_LE)

    std::cout << "running simple audio pass-through...\n";

    int loop_count = 0;
    while (true) // main processing loop
    {
        if (!process_block(
                handles.capture,
                handles.playback,
                buffer.data(),
                frames,
                loop_count))
        {
            continue; // skip this cycle
        }
    }

    snd_pcm_close(handles.capture);  // close input device
    snd_pcm_close(handles.playback); // close output device

    return 0; // exit program
}