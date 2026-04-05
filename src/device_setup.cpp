#include "device_setup.hpp"

#include <array>
#include <iostream>

namespace {

#ifndef NDEBUG
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
#else
static void log_selected_endpoint(snd_pcm_t* handle, const char* role, const std::string& device_id) {
    (void)handle;
    (void)role;
    (void)device_id;
}

static void log_pcm_capabilities(snd_pcm_t* handle, const char* role) {
    (void)handle;
    (void)role;
}
#endif

static bool supports_stream(const std::string& device, snd_pcm_stream_t stream) {
    snd_pcm_t* h = nullptr;
    int err = snd_pcm_open(&h, device.c_str(), stream, 0);
    if (err < 0) return false;
    snd_pcm_close(h);
    return true;
}

static bool format_supported(snd_pcm_t* handle, snd_pcm_format_t format) {
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    if (snd_pcm_hw_params_any(handle, params) < 0) return false;
    return snd_pcm_hw_params_test_access(
               handle, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0
        && snd_pcm_hw_params_test_format(handle, params, format) == 0;
}

} // namespace

int setup_devices_and_format(
    const std::string& preferred_capture_device,
    const std::string& fixed_playback_device,
    DeviceHandles& handles,
    std::string& selected_capture_device,
    std::string& selected_playback_device,
    snd_pcm_format_t& stream_format
) {
    handles.capture = nullptr;
    handles.playback = nullptr;

    const std::array<std::string, 2> candidates = {preferred_capture_device, fixed_playback_device};
    selected_playback_device = fixed_playback_device;
    selected_capture_device.clear();

    for (const auto& dev : candidates) {
        if (dev == fixed_playback_device && dev != preferred_capture_device) {
            continue;
        }
        if (selected_capture_device.empty() && supports_stream(dev, SND_PCM_STREAM_CAPTURE)) {
            selected_capture_device = dev;
        }
    }

    if (!supports_stream(selected_playback_device, SND_PCM_STREAM_PLAYBACK)) {
        std::cerr << "configured playback device " << selected_playback_device
                  << " is not playback-capable\n";
        return 1;
    }

    if (selected_capture_device.empty()) {
        std::cerr << "unable to assign device roles from candidates:\n";
        for (const auto& dev : candidates) {
            const bool can_cap = supports_stream(dev, SND_PCM_STREAM_CAPTURE);
            const bool can_play = supports_stream(dev, SND_PCM_STREAM_PLAYBACK);
            std::cerr << "  " << dev
                      << " capture=" << (can_cap ? "yes" : "no")
                      << " playback=" << (can_play ? "yes" : "no") << "\n";
        }
        return 1;
    }

    int err = snd_pcm_open(&handles.capture, selected_capture_device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        std::cerr << "open capture " << selected_capture_device << " failed: " << snd_strerror(err) << "\n";
        return 1;
    }

    err = snd_pcm_open(&handles.playback, selected_playback_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::cerr << "open playback " << selected_playback_device << " failed: " << snd_strerror(err) << "\n";
        snd_pcm_close(handles.capture);
        handles.capture = nullptr;
        return 1;
    }

    log_selected_endpoint(handles.capture, "capture", selected_capture_device);
    log_selected_endpoint(handles.playback, "playback", selected_playback_device);
    log_pcm_capabilities(handles.capture, "capture");
    log_pcm_capabilities(handles.playback, "playback");

    stream_format = SND_PCM_FORMAT_UNKNOWN;
    const snd_pcm_format_t format_candidates[] = {
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S16_LE,
    };

    for (snd_pcm_format_t format_candidate : format_candidates) {
        if (format_supported(handles.capture, format_candidate)
            && format_supported(handles.playback, format_candidate)) {
            stream_format = format_candidate;
            break;
        }
    }

    if (stream_format == SND_PCM_FORMAT_UNKNOWN) {
        std::cerr << "no common mmap-capable PCM format between capture and playback hw devices\n";
        snd_pcm_close(handles.capture);
        snd_pcm_close(handles.playback);
        handles.capture = nullptr;
        handles.playback = nullptr;
        return 1;
    }

    return 0;
}
