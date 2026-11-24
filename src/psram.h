#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GB_PSRAM_BASE ((uintptr_t)0x11000000u)
#define GB_PSRAM_MAX_SIZE (16u * 1024u * 1024u)
/* the PSRAM clock limit the firmware starts with, MHz (as before the menu) */
#define GB_PSRAM_DEFAULT_MHZ 133u

#ifdef __cplusplus
extern "C" {
#endif

bool gb_psram_init(void);
bool gb_psram_available(void);
size_t gb_psram_size(void);
int gb_psram_cs1_gpio(void);
void gb_psram_prepare_reset(void);
void gb_psram_reclock(void); /* after every clk_sys change */
/* the PSRAM clock limit for the next gb_psram_reclock() / gb_psram_init() */
void gb_psram_set_max_mhz(uint32_t mhz);
/* the PSRAM clock in use, Hz (0 without PSRAM) */
uint32_t gb_psram_clock_hz(void);

/* Ctrl+Alt+Del and all other watchdog resets use the same safe path. */
void gb_safe_reset(void);

#ifdef __cplusplus
}
#endif
