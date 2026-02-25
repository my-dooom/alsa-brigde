#include <alsa/asoundlib.h>   // alsa api for pcm audio capture/playback
#include <iostream>           // std::cout / std::cerr
#include <vector>             // std::vector for audio buffer
#include <cmath>              // math functions (not strictly needed here)
#include <cstdint>            // fixed-width integer types
#include <algorithm>          // std::clamp
// SPI / MCP3008
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <limits>

#define SAMPLE_RATE 48000     // audio sample rate in hz
#define CHANNELS 2            // stereo
#define FRAMES 256            // number of frames per read/write block

// SPI device used for MCP3008
#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_SPEED_HZ 1350000

// moving average configuration
#define MAX_TAPS 1024

int open_spi(const char *device)
{
    int fd = open(device, O_RDWR);
    if (fd < 0)
        return -1;

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = SPI_SPEED_HZ;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) { close(fd); return -1; }
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) { close(fd); return -1; }
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) { close(fd); return -1; }

    return fd;
}

// Read single channel (0..7) from MCP3008 via spidev (returns 0..1023), or -1 on error
int read_mcp3008(int fd, uint8_t channel)
{
    if (fd < 0 || channel > 7) return -1;

    uint8_t tx[3];
    uint8_t rx[3];
    tx[0] = 0x01; // start bit
    tx[1] = static_cast<uint8_t>((0x08 | (channel & 0x07)) << 4);
    tx[2] = 0x00;

    struct spi_ioc_transfer tr = {};
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = 3;
    tr.speed_hz = SPI_SPEED_HZ;
    tr.delay_usecs = 0;
    tr.bits_per_word = 8;

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 1) return -1;

    int value = ((rx[1] & 0x03) << 8) | rx[2];
    return value;
}

int main()
{
    snd_pcm_t *capture_handle;     // handle for input device (focusrite)
    snd_pcm_t *playback_handle;    // handle for output device (iqaudio)
    snd_pcm_hw_params_t *hw_params;// structure for hardware parameters

    int err; // used to store alsa return codes

    // ---- open capture device (focusrite) ----
    if ((err = snd_pcm_open(&capture_handle, "plughw:1,0",
                            SND_PCM_STREAM_CAPTURE, 0)) < 0)
    {
        // print error if device fails to open
        std::cerr << "cannot open capture device: "
                  << snd_strerror(err) << std::endl;
        return 1; // exit if failure
    }

    // ---- open playback device (iqaudio dac+) ----
    if ((err = snd_pcm_open(&playback_handle, "plughw:0,0",
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
                         SND_PCM_FORMAT_S32_LE); // 32-bit signed little endian
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

    std::vector<int32_t> buffer(FRAMES * CHANNELS); // audio buffer for one block (S32_LE)

    // prepare SPI / MCP3008
    int spi_fd = open_spi(SPI_DEVICE);
    if (spi_fd < 0)
        std::cerr << "warning: failed to open " << SPI_DEVICE << ", ADC disabled\n";

    std::cout << "running audio pass-through with moving-average filter...\n";

    // history stores per-channel samples for up to MAX_TAPS frames
    std::vector<std::vector<int32_t>> history(CHANNELS, std::vector<int32_t>(MAX_TAPS, 0));
    size_t history_index = 0; // next insertion index (per-frame)
    int current_taps = 16; // default

    while (true) // main processing loop
    {
        // read ADC value (channel 0) — if available
        int adc = -1;
        if (spi_fd >= 0) {
            int v = read_mcp3008(spi_fd, 0);
            if (v >= 0) adc = v;
        }

        // map ADC (0..1023) to taps (1..MAX_TAPS)
        int new_taps = current_taps;
        if (adc >= 0) {
            new_taps = 1 + (1023 - adc) * (MAX_TAPS - 1) / 1023;
            if (new_taps < 1) new_taps = 1;
            if (new_taps > MAX_TAPS) new_taps = MAX_TAPS;
        }
        current_taps = new_taps;

        // ---- read audio from focusrite into buffer ----
        err = snd_pcm_readi(capture_handle,
                            buffer.data(), FRAMES);
        if (err < 0)
        {
            // recover from xruns or other read errors
            snd_pcm_prepare(capture_handle);
            continue; // skip this cycle
        }

        // ---- apply moving-average filter controlled by ADC ----
        // history_index is per-frame; we insert samples for both channels at that slot
        for (int frame = 0; frame < FRAMES; ++frame)
        {
            // for each channel, insert sample and compute average over last current_taps frames
            for (int ch = 0; ch < CHANNELS; ++ch)
            {
                int idx = frame * CHANNELS + ch;
                int32_t sample = buffer[idx];
                history[ch][history_index] = sample;

                // compute sum of last current_taps entries
                int64_t sum = 0;
                for (int t = 0; t < current_taps; ++t)
                {
                    size_t pos = (history_index + MAX_TAPS - t) % MAX_TAPS;
                    sum += history[ch][pos];
                }
                int64_t avg = sum / current_taps;
                if (avg > std::numeric_limits<int32_t>::max()) avg = std::numeric_limits<int32_t>::max();
                if (avg < std::numeric_limits<int32_t>::min()) avg = std::numeric_limits<int32_t>::min();
                buffer[idx] = static_cast<int32_t>(avg);
            }

            history_index = (history_index + 1) % MAX_TAPS;
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