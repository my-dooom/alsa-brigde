#pragma once

#include <alsa/asoundlib.h>
#include <string>

struct DeviceHandles {
    snd_pcm_t* capture;
    snd_pcm_t* playback;
};

// Opens capture/playback devices based on project routing rules and selects a
// common mmap-capable stream format. Returns 0 on success, non-zero on failure.
int setup_devices_and_format(
    const std::string& preferred_capture_device,
    const std::string& fixed_playback_device,
    DeviceHandles& handles,
    std::string& selected_capture_device,
    std::string& selected_playback_device,
    snd_pcm_format_t& stream_format
);
