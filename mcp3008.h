#ifndef MCP3008_H
#define MCP3008_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_SPEED_HZ 1350000

int open_spi(const char *device);
int read_mcp3008(int fd, uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // MCP3008_H