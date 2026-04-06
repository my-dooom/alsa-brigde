#pragma once

#include <alsa/asoundlib.h>
#include <string>

struct DeviceHandles
{
    snd_pcm_t *capture;
    std::string capture_device_id;
    snd_pcm_t *playback;
    std::string playback_device_id;
};

// Opens capture/playback devices based on project routing rules and selects a
// common mmap-capable stream format. Returns 0 on success, non-zero on failure.
int setup_devices_and_format(
    const std::string &preferred_capture_device,
    const std::string &fixed_playback_device,
    DeviceHandles &handles,
    snd_pcm_format_t &stream_format);
