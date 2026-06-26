/* Shared NVM (flash) emulator for the v3 in-place patcher — models real SAML22 NVMCTRL
 * semantics so on-device feasibility is measurable (not the byte-addressable RAM model).
 *   - random READ is free (flash_read).
 *   - WRITE only clears bits (program); setting a 0->1 bit requires ERASING the whole ROW
 *     first, which sets the entire row to 0xFF — DESTROYING every other byte in that row.
 *   - counts erases (wear) and programs.  Page=64 B (program unit), Row=256 B (erase unit).
 * A flash-correct decoder must therefore never erase a row that still holds un-read source
 * (it must buffer/segment, or de-relocate logically, or stage in scratch). Byte-exact output
 * UNDER this emulator == flash-correct. */
#ifndef FLASH_NVM_H
#define FLASH_NVM_H
#include <stdint.h>
extern uint8_t *g_flash; extern uint32_t g_flash_n; extern uint32_t g_image_span;
uint8_t flash_read(uint32_t addr);
void    flash_write(uint32_t addr, uint8_t val);
void    nvm_init(const uint8_t *from, uint32_t from_size, uint32_t span);
long    nvm_erases(void);     /* total row erases (wear) */
long    nvm_programs(void);   /* total byte programs */
uint32_t nvm_rows(void);      /* distinct rows erased */
uint32_t nvm_rows_amplified(void); /* rows erased >1 time — MUST be 0 (no write amplification) */
uint32_t nvm_max_row_erases(void); /* max erases on any single row — MUST be <=1 */
long     nvm_frontier_inversions(void); /* req #2: row-finalize direction reversals — MUST be 0 (sequential) */
#endif
