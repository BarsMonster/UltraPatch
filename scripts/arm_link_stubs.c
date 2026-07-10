/* Minimal platform symbols for the no-startup decoder footprint link. */
#include <stdint.h>

uint8_t flash_read(uint32_t addr)
{
	(void)addr;
	return 0;
}

void flash_write(uint32_t addr, uint8_t value)
{
	(void)addr;
	(void)value;
}
