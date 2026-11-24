#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The flash setting the firmware starts with.
 * RP2350: the flash clock limit in MHz; every flash chip seen on the boards
 * so far runs at 66 (as murm386).
 * RP2040: the XIP SSI divider itself (even, 2..254); 4 is the boot2 divider
 * these firmwares have always used (PICO_FLASH_SPI_CLKDIV). */
#if PICO_RP2350
#define CLOCKS_FLASH_DEFAULT 66u
#else
#define CLOCKS_FLASH_DEFAULT 4u
#endif

/* The core voltage chosen by the CPU clock (RP2350: above 378 MHz 1.70 V,
 * otherwise 1.60 V); any other value is an enum vreg_voltage. */
#define CLOCKS_VOLTAGE_AUTO (-1)

/* Set clk_sys to cpu_mhz, the flash (see CLOCKS_FLASH_DEFAULT) and, on
 * RP2350, the core voltage: raised before a faster clock, lowered after a
 * slower one.  RP2040 ignores the voltage: the highest one (1.30 V), as before.
 * The caller keeps core 1 out of the flash (multicore lockout) once it runs,
 * and sets again every divider derived from clk_sys afterwards.
 * Returns false, with nothing changed, when the PLL cannot make cpu_mhz. */
bool clocks_set(uint32_t cpu_mhz, uint32_t flash, int voltage);

/* The flash clock in use, Hz. */
uint32_t clocks_flash_hz(void);

/* The core voltage in use, mV (0 before clocks_set()). */
uint32_t clocks_voltage_mv(void);

/* flash_range_erase/program and flash_do_cmd leave XIP through the boot-time
 * setup (RP2040: the boot2 SSI divider); call this right after them, from RAM,
 * with nothing running from the flash in between. */
void clocks_flash_restore(void);

#ifdef __cplusplus
}
#endif
