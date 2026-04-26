#include <alsa/asoundlib.h> // alsa api for pcm audio capture/playback
#include <atomic>
#include <cstdlib>          // getenv, strtoul
#include <iostream>         // std::cout / std::cerr
#include <linux/spi/spidev.h>
#include <string>           // std::string for device names
#include <thread>
#include <unistd.h>         // close

#include "audio_config.hpp"
#include "device_setup.hpp"
#include "mcp3008.h"
#include "process_audio.hpp"
#ifdef ENABLE_LINK_SYNC
#include "link_sync.hpp"
#endif

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

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_CHANNEL_1 0
#define SPI_CHANNEL_2 1
#define PARAM_UPDATE_EVERY_LOOPS 4

// SPI reader thread — polls all channels as fast as the bus allows.
static std::atomic<bool> g_spi_running{false};
static std::atomic<int>  g_spi_raw[MCP3008_CHANNELS]; // -1 until first valid read

static void spi_reader_loop(int fd)
{
    uint16_t buf[MCP3008_CHANNELS];
    while (g_spi_running.load(std::memory_order_relaxed)) {
        if (read_mcp3008_all(fd, buf) == 0) {
            for (int i = 0; i < MCP3008_CHANNELS; ++i)
                g_spi_raw[i].store(static_cast<int>(buf[i]), std::memory_order_relaxed);
        }
    }
}

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
    set_effect_target_params(EffectParams{1.0f, 1.0f, 2.0f, 0.95f});

#ifdef ENABLE_LINK_SYNC
    link_sync_start();
#endif

    const char* spi_device_env = std::getenv("AUDIO_BRIDGE_SPI_DEVICE");
    const char* spi_speed_env = std::getenv("AUDIO_BRIDGE_SPI_SPEED_HZ");

    uint32_t spi_speed = SPI_SPEED_HZ;
    if (spi_speed_env && spi_speed_env[0] != '\0') {
        char* end = nullptr;
        unsigned long parsed = std::strtoul(spi_speed_env, &end, 10);
        if (end != spi_speed_env && *end == '\0' && parsed > 0UL) {
            spi_speed = static_cast<uint32_t>(parsed);
        }
    }

    const char* spi_device = (spi_device_env && spi_device_env[0] != '\0')
        ? spi_device_env
        : SPI_DEVICE;

    mcp3008_spi_config spi_cfg{};
    spi_cfg.device = spi_device;
    spi_cfg.speed_hz = spi_speed;
    spi_cfg.mode = SPI_MODE_0;
    spi_cfg.bits_per_word = 8;

    int spi_fd = open_spi_config(&spi_cfg);
    if (spi_fd < 0)
    {
        std::cerr << "warning: failed to open SPI device " << spi_device
                  << ", running with default effect params\n";
    }
    else
    {
        AV_DEBUG_LOG(std::cout << "MCP3008 SPI ready: device=" << spi_device
                               << " speed_hz=" << spi_speed << "\n";);
    }

    // Initialise SPI atomics and start reader thread.
    for (int i = 0; i < MCP3008_CHANNELS; ++i)
        g_spi_raw[i].store(-1, std::memory_order_relaxed);
    std::thread spi_thread;
    if (spi_fd >= 0) {
        g_spi_running.store(true, std::memory_order_relaxed);
        spi_thread = std::thread(spi_reader_loop, spi_fd);
    }

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
        if ((loop_count % PARAM_UPDATE_EVERY_LOOPS) == 0)
        {
            const int channel_1_raw = g_spi_raw[SPI_CHANNEL_1].load(std::memory_order_relaxed);
            const int channel_2_raw = g_spi_raw[SPI_CHANNEL_2].load(std::memory_order_relaxed);

            // Wet hardcoded to 1.0 — reverb used as send effect.
            // MCP ch0 → reverb decay time [0.1, 10] s
            // MCP ch1 → output gain [0, 2]
            constexpr float kMcpMax = 1023.0f;
            const float decay = (channel_1_raw >= 0)
                ? 0.1f + (static_cast<float>(channel_1_raw) / kMcpMax) * 9.9f
                : 2.0f;
            const float gain  = (channel_2_raw >= 0)
                ? (static_cast<float>(channel_2_raw) / kMcpMax) * 2.0f
                : 1.0f;
            const int channel_3_raw = g_spi_raw[2].load(std::memory_order_relaxed);
            const float damping = (channel_3_raw >= 0)
                ? (static_cast<float>(channel_3_raw) / kMcpMax) * 0.9999f
                : 0.95f;
            set_effect_target_params(EffectParams{gain, 1.0f, decay, damping});

#ifdef ENABLE_LINK_SYNC
            {
                const float link_bpm = link_sync_get_bpm();
                if (link_bpm > 0.0f) {
                    // beat_samples = samples per beat at current Link tempo
                    // e.g. at 128 BPM: 48000*60/128 = 22500 samples
                    const float beat_samples =
                        (static_cast<float>(SAMPLE_RATE) * 60.0f) / link_bpm;
                    (void)beat_samples; // wire to effect param when delay is added
                    AV_DEBUG_LOG(std::cout << "Link BPM: " << link_bpm
                                          << "  beat_samples: " << beat_samples << "\n";);
                }
            }
#endif

#ifndef NDEBUG
            {
                std::cerr << "\rmcp raw";
                for (int ch = 0; ch < MCP3008_CHANNELS; ++ch) {
                    std::cerr << " channel_" << (ch + 1) << "=" << g_spi_raw[ch].load(std::memory_order_relaxed);
                }
                std::cerr << "   " << std::flush;
            }
#endif
        }

        if (!process_block(
                handles.capture,
                handles.playback,
                frames,
                loop_count))
        {
            continue; // skip this cycle
        }
    }

    if (spi_thread.joinable()) {
        g_spi_running.store(false, std::memory_order_relaxed);
        spi_thread.join();
    }
    if (spi_fd >= 0)
    {
        close(spi_fd);
    }

#ifdef ENABLE_LINK_SYNC
    link_sync_stop();
#endif

    snd_pcm_close(handles.capture);  // close input device
    snd_pcm_close(handles.playback); // close output device

    return 0; // exit program
}