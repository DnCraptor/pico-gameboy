/* clk_sys, the core voltage and the flash clock together.
 *
 * The flash clock is clk_sys divided by an integer, so a firmware built for
 * 378 MHz and one built for 504 MHz ran the same chip at different clocks,
 * and the overclocking menu moved it further (432 MHz / 3 = 144 MHz) without
 * telling.  Here the flash gets a limit in MHz instead (the murm386 way) and
 * the divisor follows clk_sys. */

#include "clocks.h"

#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/vreg.h>
#if PICO_RP2350
#include <hardware/structs/qmi.h>
#else
#include <hardware/structs/ssi.h>
#include <hardware/structs/vreg_and_chip_reset.h>
#endif

static uint32_t flash_max_mhz = CLOCKS_FLASH_DEFAULT_MHZ;
static uint32_t flash_divisor;  /* in use; 0 until clocks_set() */
static uint32_t clk_hz;         /* clk_sys for clocks_flash_restore() (no flash calls there) */

/* The divisor for a flash clock of at most flash_max_mhz; up to 1/16 above
 * the limit is accepted, as in murm386 (504 / 3 = 168 for 166 MHz). */
static uint32_t __not_in_flash_func(divisor_for)(const uint32_t clock_hz) {
    const uint32_t max_hz = flash_max_mhz * 1000000u;
    uint32_t d = (clock_hz + max_hz - (max_hz >> 4) - 1u) / max_hz;
    if (d < 2u) d = 2u;
#if !PICO_RP2350
    d += d & 1u; /* SSI BAUDR: even values only */
#endif
    if (d > 254u) d = 254u;
    return d;
}

#if PICO_RP2350
/* QMI window 0.  RXDELAY (in half clk_sys cycles) stays about 4 ns at any
 * clk_sys: the delay of the timings these firmwares have always used
 * (252 MHz: 2, 378 MHz: 3, 504 MHz: 4), never later than half an SCK period
 * (the divisor) and at most 7 (the field width). */
static void __no_inline_not_in_flash_func(flash_timing_set)(const uint32_t clock_hz, const uint32_t d) {
    uint32_t rx = (clock_hz + 62500000u) / 125000000u;
    if (rx < 1u) rx = 1u;
    if (rx > d) rx = d;
    if (rx > 7u) rx = 7u;
    qmi_hw->m[0].timing = 0x60007000u |
                          rx << QMI_M0_TIMING_RXDELAY_LSB |
                          d << QMI_M0_TIMING_CLKDIV_LSB;
    __asm volatile ("dsb; isb" ::: "memory");
}
#else
/* XIP SSI: BAUDR can be written only while the SSI is disabled; the XIP setup
 * of boot2 (frame format, continuous read) stays as it is. */
static void __no_inline_not_in_flash_func(flash_timing_set)(const uint32_t clock_hz, const uint32_t d) {
    (void)clock_hz;
    ssi_hw->ssienr = 0;
    ssi_hw->baudr = d;
    ssi_hw->ssienr = 1;
}
#endif

/* Runs from RAM: the flash timing changes under it.  While clk_sys moves the
 * flash keeps the divisor of the higher of the two clocks, which is right
 * (or slower) for both; set_sys_clock_pll() itself may run from the flash. */
static void __no_inline_not_in_flash_func(clocks_switch)(const uint32_t old_hz, const uint32_t new_hz,
                                                         const uint32_t vco, const uint pd1, const uint pd2) {
    const uint32_t ints = save_and_disable_interrupts();
    const uint32_t hi = new_hz > old_hz ? new_hz : old_hz;
    flash_timing_set(hi, divisor_for(hi));
    if (new_hz != old_hz)
        set_sys_clock_pll(vco, pd1, pd2);
    clk_hz = new_hz;
    flash_divisor = divisor_for(new_hz);
    flash_timing_set(new_hz, flash_divisor);
    restore_interrupts(ints);
}

bool clocks_set(const uint32_t cpu_mhz, const uint32_t flash_mhz) {
    uint vco, pd1, pd2;
    if (cpu_mhz == 0 || flash_mhz == 0 ||
        !check_sys_clock_khz(cpu_mhz * 1000u, &vco, &pd1, &pd2))
        return false;

    const uint32_t old_hz = clock_get_hz(clk_sys);
    const uint32_t new_hz = cpu_mhz * 1000000u;
    const bool raising = new_hz > old_hz;
#if PICO_RP2350
    const enum vreg_voltage voltage = cpu_mhz > 378u ? VREG_VOLTAGE_1_70 : VREG_VOLTAGE_1_60;
    vreg_disable_voltage_limit();
    if (raising) {
        vreg_set_voltage(voltage);
        sleep_ms(10);
    }
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    if (raising) sleep_ms(33);
#endif

    flash_max_mhz = flash_mhz;
    clocks_switch(old_hz, new_hz, vco, pd1, pd2);

#if PICO_RP2350
    if (!raising) vreg_set_voltage(voltage);
#endif
    return true;
}

uint32_t clocks_flash_hz(void) {
    return flash_divisor ? clk_hz / flash_divisor : 0;
}

void __no_inline_not_in_flash_func(clocks_flash_restore)(void) {
    if (flash_divisor) flash_timing_set(clk_hz, flash_divisor);
}
