#include "mcp3008.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

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

int read_mcp3008(int fd, uint8_t channel)
{
    if (fd < 0 || channel > 7) return -1;

    uint8_t tx[3];
    uint8_t rx[3];
    tx[0] = 0x01; // start bit
    tx[1] = (uint8_t)((0x08 | (channel & 0x07)) << 4);
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