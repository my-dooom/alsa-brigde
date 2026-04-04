#include <alsa/asoundlib.h>   // alsa api for pcm audio capture/playback
#include <iostream>           // std::cout / std::cerr
#include <string>             // std::string for device names
#include <utility>            // std::swap

#include "process_audio.hpp"

#define SAMPLE_RATE 48000     // audio sample rate in hz
#define CHANNELS 2            // stereo
#define PERIOD_FRAMES 512     // target period size in frames
#define BUFFER_PERIODS 4      // total ring buffer = PERIOD_FRAMES * BUFFER_PERIODS

struct DeviceHandles
{
    snd_pcm_t* capture;
    snd_pcm_t* playback;
};

static void log_selected_endpoint(snd_pcm_t* handle, const char* role, const std::string& device_id) {
    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);
    int err = snd_pcm_info(handle, info);
    if (err < 0) {
        std::cerr << role << " selected=" << device_id
                  << " info query failed: " << snd_strerror(err) << "\n";
        return;
    }

    std::cout << role << " selected=" << device_id
              << " name='" << snd_pcm_info_get_name(info)
              << "' subdevice='" << snd_pcm_info_get_subdevice_name(info)
              << "'\n";
}

static void log_pcm_capabilities(snd_pcm_t* handle, const char* role) {
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    int err = snd_pcm_hw_params_any(handle, params);
    if (err < 0) {
     std::cerr << role << " probe hw_params_any failed: " << snd_strerror(err) << "\n";
     return;
    }

    auto yes_no = [](bool v) -> const char* { return v ? "yes" : "no"; };
    std::cout << role << " supports mmap_interleaved="
        << yes_no(snd_pcm_hw_params_test_access(
            handle, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0)
        << ", rw_interleaved="
        << yes_no(snd_pcm_hw_params_test_access(
            handle, params, SND_PCM_ACCESS_RW_INTERLEAVED) == 0)
        << "\n";

    std::cout << role << " supports format FLOAT_LE="
        << yes_no(snd_pcm_hw_params_test_format(
            handle, params, SND_PCM_FORMAT_FLOAT_LE) == 0)
        << ", S32_LE="
        << yes_no(snd_pcm_hw_params_test_format(
            handle, params, SND_PCM_FORMAT_S32_LE) == 0)
        << ", S24_LE="
        << yes_no(snd_pcm_hw_params_test_format(
            handle, params, SND_PCM_FORMAT_S24_LE) == 0)
        << ", S16_LE="
        << yes_no(snd_pcm_hw_params_test_format(
            handle, params, SND_PCM_FORMAT_S16_LE) == 0)
        << "\n";
}

int main()
{

    std::string capture_device = "hw:0,0";
    std::string playback_device = "hw:1,0";

    DeviceHandles handles; 
    snd_pcm_hw_params_t *hw_params;// structure for hardware parameters

    int err; // used to store alsa return codes
    snd_pcm_uframes_t frames;
    snd_pcm_uframes_t buffer_frames;
    unsigned int val;
    int dir;

    // ---- open capture device (focusrite) ----
    bool swapped = false;

    auto try_open = [&](const std::string& cap, const std::string& play) -> bool {
        int e = snd_pcm_open(&handles.capture, cap.c_str(),
                             SND_PCM_STREAM_CAPTURE, 0);
        if (e < 0) {
            std::cerr << "open capture " << cap << " failed: " << snd_strerror(e) << "\n";
            return false;
        }
        e = snd_pcm_open(&handles.playback, play.c_str(),
                         SND_PCM_STREAM_PLAYBACK, 0);
        if (e < 0) {
            std::cerr << "open playback " << play << " failed: " << snd_strerror(e) << "\n";
            snd_pcm_close(handles.capture);
            return false;
        }
        return true;
    };

    // Try hw devices only (DMA path only).
    if (!try_open(capture_device, playback_device)) {
        std::swap(capture_device, playback_device);
        swapped = true;
        if (!try_open(capture_device, playback_device)) {
            std::cerr << "cannot open hw devices in either direction\n";
            return 1;
        }
    }

    if (swapped) {
        std::cout << "Devices were swapped: capture=" << capture_device << ", playback=" << playback_device << "\n";
    }
    log_selected_endpoint(handles.capture, "capture", capture_device);
    log_selected_endpoint(handles.playback, "playback", playback_device);
    std::cout << "Access mode: mmap (DMA only)\n";

    log_pcm_capabilities(handles.capture, "capture");
    log_pcm_capabilities(handles.playback, "playback");

    auto format_supported = [](snd_pcm_t* handle, snd_pcm_format_t fmt) -> bool {
        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca(&params);
        if (snd_pcm_hw_params_any(handle, params) < 0) return false;
        return snd_pcm_hw_params_test_access(
                   handle, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0
            && snd_pcm_hw_params_test_format(handle, params, fmt) == 0;
    };

    snd_pcm_format_t stream_format = SND_PCM_FORMAT_UNKNOWN;
    const snd_pcm_format_t candidates[] = {
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S16_LE
    };
    for (snd_pcm_format_t candidate : candidates) {
        if (format_supported(handles.capture, candidate)
            && format_supported(handles.playback, candidate)) {
            stream_format = candidate;
            break;
        }
    }

    if (stream_format == SND_PCM_FORMAT_UNKNOWN) {
        std::cerr << "no common mmap-capable PCM format between capture and playback hw devices\n";
        return 1;
    }

    std::cout << "Selected DMA format: " << snd_pcm_format_name(stream_format) << "\n";
    set_stream_format(stream_format);

    // ---- helper lambda to configure an alsa pcm device ----
    auto configure_device = [&](snd_pcm_t *handle,
                                const char* role,
                                bool relaxed_timing) -> int
    {
        snd_pcm_hw_params_alloca(&hw_params);                 // allocate hw params struct on stack
        int e = snd_pcm_hw_params_any(handle, hw_params);     // initialize with default values
        if (e < 0) {
            std::cerr << role << " hw_params_any failed: " << snd_strerror(e) << "\n";
            return e;
        }

        snd_pcm_access_t access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
        e = snd_pcm_hw_params_set_access(handle, hw_params, access);
        if (e < 0) {
            std::cerr << role << " set_access failed: " << snd_strerror(e) << "\n";
            return e;
        }

        e = snd_pcm_hw_params_set_format(handle, hw_params,
                         stream_format);
        if (e < 0) {
            std::cerr << role << " set_format(" << snd_pcm_format_name(stream_format)
                      << ") failed: " << snd_strerror(e) << "\n";
            return e;
        }
                // Enable ALSA resampling support when available.
                unsigned int resample = 1;
                snd_pcm_hw_params_set_rate_resample(handle, hw_params, resample);

                    /* 48000 bits/second sampling rate */
        val = SAMPLE_RATE;
                e = snd_pcm_hw_params_set_rate_near(handle, hw_params,
                                                                                        &val, &dir);
                if (e < 0) {
                    std::cerr << role << " set_rate_near failed: " << snd_strerror(e) << "\n";
                    return e;
                }

                if (!relaxed_timing) {
                    frames = PERIOD_FRAMES;
                    e = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &frames, &dir);
                    if (e < 0) {
                        std::cerr << role << " set_period_size_near failed: " << snd_strerror(e) << "\n";
                        return e;
                    }

                    buffer_frames = PERIOD_FRAMES * BUFFER_PERIODS;
                    e = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_frames);
                    if (e < 0) {
                        std::cerr << role << " set_buffer_size_near failed: " << snd_strerror(e) << "\n";
                        return e;
                    }
                }

                e = snd_pcm_hw_params_set_channels(handle, hw_params,
                                                                                     CHANNELS);          // set number of channels
                if (e < 0) {
                    std::cerr << role << " set_channels failed: " << snd_strerror(e) << "\n";
                    return e;
                }

        e = snd_pcm_hw_params(handle, hw_params);             // apply parameters to device
        if (e < 0) {
            std::cerr << role << " hw_params apply failed: " << snd_strerror(e) << "\n";
            return e;
        }
        return 0;
    };

    // DMA-only setup: try strict timing first, then relaxed timing.
    int cap_cfg_err = configure_device(handles.capture, "capture", false);
    int play_cfg_err = configure_device(handles.playback, "playback", false);
    if (cap_cfg_err < 0 || play_cfg_err < 0) {
        std::cerr << "Initial configure failed: capture=" << cap_cfg_err
                  << " (" << (cap_cfg_err < 0 ? snd_strerror(cap_cfg_err) : "ok") << ")"
                  << ", playback=" << play_cfg_err
                  << " (" << (play_cfg_err < 0 ? snd_strerror(play_cfg_err) : "ok") << ")\n";

        if (cap_cfg_err < 0 || play_cfg_err < 0) {
            std::cerr << "Strict config failed, retrying relaxed timing...\n";
            cap_cfg_err = configure_device(handles.capture, "capture", true);
            play_cfg_err = configure_device(handles.playback, "playback", true);
        }

        if (cap_cfg_err < 0 || play_cfg_err < 0) {
            std::cerr << "Fallback configure failed: capture=" << cap_cfg_err
                      << " (" << (cap_cfg_err < 0 ? snd_strerror(cap_cfg_err) : "ok") << ")"
                      << ", playback=" << play_cfg_err
                      << " (" << (play_cfg_err < 0 ? snd_strerror(play_cfg_err) : "ok") << ")\n";
            std::cerr << "DMA hw configuration failed\n";
            return 1;
        }
    }

    err = snd_pcm_prepare(handles.capture);    // prepare capture device for use
    if (err < 0) {
        std::cerr << "capture prepare failed: " << snd_strerror(err) << "\n";
        return 1;
    }

    err = snd_pcm_prepare(handles.playback);   // prepare playback device for use
    if (err < 0) {
        std::cerr << "playback prepare failed: " << snd_strerror(err) << "\n";
        return 1;
    }

    std::cout << "Using independent capture/playback streams (no PCM link)\n";

    /* Use a buffer large enough to hold one period */
    snd_pcm_hw_params_get_period_size(hw_params, &frames,
                                      &dir);
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames);
    std::cout << "Configured period=" << frames
              << " frames, buffer=" << buffer_frames << " frames\n";

    // Start capture first; playback is started in the mmap loop after first commit.
    err = snd_pcm_start(handles.capture);
    if (err < 0) {
        std::cerr << "capture start failed: " << snd_strerror(err)
                  << ", retrying prepare/start...\n";
        err = snd_pcm_prepare(handles.capture);
        if (err < 0) {
            std::cerr << "capture prepare retry failed: " << snd_strerror(err) << "\n";
            return 1;
        }
        err = snd_pcm_start(handles.capture);
        if (err < 0) {
            std::cerr << "capture start retry failed: " << snd_strerror(err) << "\n";
            return 1;
        }
    }
    std::cout << "running mmap DMA audio pass-through...\n";

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