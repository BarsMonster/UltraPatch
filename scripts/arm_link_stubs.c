/* Minimal platform symbols for the no-startup decoder footprint link. */
#include <stdint.h>
#include "patch_config.h"

uint8_t flash_read(uint32_t addr)
{
	(void)addr;
	return 0;
}

void flash_write_page(uint32_t addr, const uint8_t page[OUTROW])
{
	(void)addr;
	(void)page;
}
