#include "mcp3008.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

/*
 * open_spi - Open and configure an SPI device for MCP3008 communication.
 *
 * Sets SPI mode 0 (CPOL=0, CPHA=0), 8 bits per word, and the clock speed
 * defined by SPI_SPEED_HZ.  Returns the file descriptor on success, -1 on
 * failure (the fd is closed before returning on any ioctl error).
 */
int open_spi(const char *device)
{
    mcp3008_spi_config config;
    config.device = device;
    config.speed_hz = SPI_SPEED_HZ;
    config.mode = SPI_MODE_0;
    config.bits_per_word = 8;
    return open_spi_config(&config);
}

int open_spi_config(const mcp3008_spi_config* config)
{
    if (!config || !config->device) return -1;

    int fd = open(config->device, O_RDWR);
    if (fd < 0)
        return -1;

    uint8_t mode = config->mode;
    uint8_t bits = config->bits_per_word;
    uint32_t speed = config->speed_hz;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode)          < 0) { close(fd); return -1; }
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) { close(fd); return -1; }
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) { close(fd); return -1; }

    return fd;
}

/*
 * read_mcp3008 - Perform a single-ended 10-bit ADC read from an MCP3008 channel.
 *
 * SPI frame (3 bytes, MSB-first):
 *
 *   TX byte 0:  0x01              – start bit
 *   TX byte 1:  (1 D2 D1 D0) << 4 – single-ended mode (bit 3 = 1) | channel
 *   TX byte 2:  0x00              – dummy byte to clock out the result
 *
 *   RX byte 0:  (don't care)
 *   RX byte 1:  ------XX          – high 2 bits of the 10-bit result
 *   RX byte 2:  XXXXXXXX          – low  8 bits of the 10-bit result
 *
 * Returns 0–1023 on success, -1 on error.
 */
int read_mcp3008(int fd, uint8_t channel)
{
    if (fd < 0 || channel > 7) return -1;

    /* Build the 3-byte SPI command for a single-ended read. */
    uint8_t tx[3] = {
        0x01,                                       /* start bit            */
        (uint8_t)((0x08 | (channel & 0x07)) << 4),  /* single-ended | ch    */
        0x00                                         /* dummy clock byte     */
    };
    uint8_t rx[3] = {0};

    /* Zero the transfer struct so all optional fields are safely defaulted. */
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf        = (unsigned long)tx;
    tr.rx_buf        = (unsigned long)rx;
    tr.len           = sizeof(tx);
    tr.speed_hz      = SPI_SPEED_HZ;
    tr.bits_per_word = 8;

    /* Execute one full-duplex SPI message. */
    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < (int)sizeof(tx)) return -1;  /* short or failed transfer */

    /* Reconstruct the 10-bit value from the last two received bytes. */
    return ((rx[1] & 0x03) << 8) | rx[2];
}

int read_mcp3008_multi(int fd, const uint8_t* channels, uint8_t count, uint16_t* out_values)
{
    if (fd < 0 || !channels || !out_values || count == 0) return -1;

    for (uint8_t i = 0; i < count; ++i) {
        int v = read_mcp3008(fd, channels[i]);
        if (v < 0) return -1;
        out_values[i] = (uint16_t)v;
    }

    return 0;
}

int read_mcp3008_all(int fd, uint16_t out_values[MCP3008_CHANNELS])
{
    static const uint8_t k_all_channels[MCP3008_CHANNELS] = {0, 1, 2, 3, 4, 5, 6, 7};
    return read_mcp3008_multi(fd, k_all_channels, MCP3008_CHANNELS, out_values);
}

/*
 * map_value - Linearly map an integer from one range to another.
 *
 * Equivalent to the Arduino map() function:
 *   result = (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min
 *
 * Typical use with the MCP3008 (0–1023 ADC range):
 *   int percent = map_value(adc, 0, 1023, 0, 100);
 *   int voltage_mv = map_value(adc, 0, 1023, 0, 3300);
 *
 * Uses long arithmetic internally to avoid overflow on 16/32-bit targets.
 * Returns out_min when in_min == in_max to avoid division by zero.
 */
long map_value(long value, long in_min, long in_max, long out_min, long out_max)
{
    if (in_max == in_min) return out_min;
    return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}