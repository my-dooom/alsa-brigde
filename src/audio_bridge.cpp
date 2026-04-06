#include <alsa/asoundlib.h> // alsa api for pcm audio capture/playback
#include <iostream>         // std::cout / std::cerr
#include <string>           // std::string for device names

#include "audio_config.hpp"
#include "device_setup.hpp"
#include "process_audio.hpp"

#ifndef NDEBUG
#define AV_DEBUG_LOG(stmt) \
    do                     \
    {                      \
        stmt;              \
    } while (0)
#else
#define AV_DEBUG_LOG(stmt) \
    do                     \
    {                      \
    } while (0)
#endif

#define SAMPLE_RATE 48000 // audio sample rate in hz
#define CHANNELS 2        // stereo
#define PERIOD_FRAMES 512 // target period size in frames
#define BUFFER_PERIODS 4  // total ring buffer = PERIOD_FRAMES * BUFFER_PERIODS

int main()
{

    std::string capture_device = "hw:CARD=USB,DEV=0";
    std::string playback_device = "hw:CARD=DAC,DEV=0";

    DeviceHandles handles;
    int err; // used to store alsa return codes
    snd_pcm_uframes_t frames;
    snd_pcm_uframes_t buffer_frames;

    snd_pcm_format_t stream_format = SND_PCM_FORMAT_UNKNOWN;

    err = setup_devices_and_format(
        capture_device,
        playback_device,
        handles,
        stream_format);
    if (err != 0)
    {
        return 1;
    }

    if (stream_format == SND_PCM_FORMAT_UNKNOWN)
    {
        std::cerr << "no common mmap-capable PCM format between capture and playback hw devices\n";
        return 1;
    }

    AV_DEBUG_LOG(std::cout << "Selected DMA format: " << snd_pcm_format_name(stream_format) << "\n";);
    set_stream_format(stream_format);

    // Configure both endpoints with fixed timing settings.
    int cap_cfg_err = configure_device(
        handles.capture,
        "capture",
        stream_format,
        SAMPLE_RATE,
        CHANNELS,
        PERIOD_FRAMES,
        BUFFER_PERIODS);
    int play_cfg_err = configure_device(
        handles.playback,
        "playback",
        stream_format,
        SAMPLE_RATE,
        CHANNELS,
        PERIOD_FRAMES,
        BUFFER_PERIODS);
    if (cap_cfg_err < 0 || play_cfg_err < 0)
    {
        std::cerr << "Configure failed: capture=" << cap_cfg_err
                  << " (" << (cap_cfg_err < 0 ? snd_strerror(cap_cfg_err) : "ok") << ")"
                  << ", playback=" << play_cfg_err
                  << " (" << (play_cfg_err < 0 ? snd_strerror(play_cfg_err) : "ok") << ")\n";
        std::cerr << "DMA hw configuration failed\n";
        return 1;
    }

    err = snd_pcm_prepare(handles.capture); // prepare capture device for use
    if (err < 0)
    {
        std::cerr << "capture prepare failed: " << snd_strerror(err) << "\n";
        return 1;
    }

    err = snd_pcm_prepare(handles.playback); // prepare playback device for use
    if (err < 0)
    {
        std::cerr << "playback prepare failed: " << snd_strerror(err) << "\n";
        return 1;
    }

    // Read effective period/buffer values chosen by ALSA after hw_params negotiation.
    err = snd_pcm_get_params(handles.playback, &buffer_frames, &frames);
    if (err < 0)
    {
        std::cerr << "snd_pcm_get_params failed: " << snd_strerror(err) << "\n";
        return 1;
    }
    AV_DEBUG_LOG(std::cout << "Configured period=" << frames
                           << " frames, buffer=" << buffer_frames << " frames\n";);

    // Start capture first; playback is started in the mmap loop after first commit.
    err = snd_pcm_start(handles.capture);
    if (err < 0)
    {
        std::cerr << "capture start failed: " << snd_strerror(err)
                  << ", retrying prepare/start...\n";
        err = snd_pcm_prepare(handles.capture);
        if (err < 0)
        {
            std::cerr << "capture prepare retry failed: " << snd_strerror(err) << "\n";
            return 1;
        }
        err = snd_pcm_start(handles.capture);
        if (err < 0)
        {
            std::cerr << "capture start retry failed: " << snd_strerror(err) << "\n";
            return 1;
        }
    }
    AV_DEBUG_LOG(std::cout << "running mmap DMA audio pass-through...\n";);

    int loop_count = 0;
    while (true) // main processing loop
    {
        if (!process_block(
                handles.capture,
                handles.playback,
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