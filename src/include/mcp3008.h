#ifndef MCP3008_H
#define MCP3008_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_SPEED_HZ 1350000
#define MCP3008_CHANNELS 8

typedef struct {
	const char* device;
	uint32_t speed_hz;
	uint8_t mode;
	uint8_t bits_per_word;
} mcp3008_spi_config;

int  open_spi(const char *device);
int  open_spi_config(const mcp3008_spi_config* config);
int  read_mcp3008(int fd, uint8_t channel);
int  read_mcp3008_multi(int fd, const uint8_t* channels, uint8_t count, uint16_t* out_values);
int  read_mcp3008_all(int fd, uint16_t out_values[MCP3008_CHANNELS]);
long map_value(long value, long in_min, long in_max, long out_min, long out_max);

#ifdef __cplusplus
}
#endif

#endif // MCP3008_H