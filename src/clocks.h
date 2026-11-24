#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The flash clock limit the firmware starts with, MHz: every flash chip seen
 * on the boards so far runs at it (as murm386).  The menu raises it. */
#define CLOCKS_FLASH_DEFAULT_MHZ 66u

/* Set clk_sys to cpu_mhz and the flash clock to at most flash_max_mhz.
 * RP2350: the core voltage follows clk_sys (above 378 MHz 1.70 V, otherwise
 * 1.60 V), raised before a faster clock and lowered after a slower one.
 * RP2040: the core voltage is the highest one (1.30 V), as before.
 * The caller keeps core 1 out of the flash (multicore lockout) once it runs,
 * and sets again every divider derived from clk_sys afterwards.
 * Returns false, with nothing changed, when the PLL cannot make cpu_mhz. */
bool clocks_set(uint32_t cpu_mhz, uint32_t flash_max_mhz);

/* The flash clock in use, Hz. */
uint32_t clocks_flash_hz(void);

/* flash_range_erase/program and flash_do_cmd leave XIP through the boot-time
 * setup (RP2040: the boot2 SSI divider); call this right after them, from RAM,
 * with nothing running from the flash in between. */
void clocks_flash_restore(void);

#ifdef __cplusplus
}
#endif
