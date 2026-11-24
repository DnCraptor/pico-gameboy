/* clk_sys, the core voltage and the flash clock together.
 *
 * The flash clock is clk_sys divided by an integer, so a firmware built for
 * 378 MHz and one built for 504 MHz ran the same chip at different clocks,
 * and the overclocking menu moved it further (432 MHz / 3 = 144 MHz) without
 * telling.  RP2350: the flash gets a limit in MHz instead (the murm386 way)
 * and the divisor follows clk_sys.  RP2040: the SSI divider is chosen as is
 * (4 by default, as boot2 always set it). */

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

static uint32_t flash_setting = CLOCKS_FLASH_DEFAULT;
static uint32_t flash_divisor;  /* in use; 0 until clocks_set() */
static uint32_t clk_hz;         /* clk_sys for clocks_flash_restore() (no flash calls there) */

#if PICO_RP2350
/* The divisor for a flash clock of at most flash_setting MHz; up to 1/16
 * above the limit is accepted, as in murm386 (504 / 3 = 168 for 166 MHz). */
static uint32_t __not_in_flash_func(divisor_for)(const uint32_t clock_hz) {
    const uint32_t max_hz = flash_setting * 1000000u;
    uint32_t d = (clock_hz + max_hz - (max_hz >> 4) - 1u) / max_hz;
    if (d < 2u) d = 2u;
    if (d > 255u) d = 255u;
    return d;
}

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

static int voltage_in_use = -1; /* enum vreg_voltage; -1: not set by us yet (boot 1.10 V) */

static enum vreg_voltage voltage_for(const uint32_t cpu_mhz, const int voltage) {
    if (voltage != CLOCKS_VOLTAGE_AUTO) return (enum vreg_voltage)voltage;
    return cpu_mhz > 378u ? VREG_VOLTAGE_1_70 : VREG_VOLTAGE_1_60;
}

/* mV of an enum vreg_voltage: 0.55 V + 50 mV steps up to 1.30 V, then the table */
static uint32_t voltage_mv(const int v) {
    static const uint16_t high[] = { 1350, 1400, 1500, 1600, 1650, 1700, 1800, 1900, 2000,
                                     2350, 2500, 2650, 2800, 3000, 3150, 3300 };
    if (v < 0) return 1100;
    if (v <= VREG_VOLTAGE_1_30) return 550u + 50u * (uint32_t)v;
    const unsigned i = (unsigned)v - VREG_VOLTAGE_1_30 - 1u;
    return i < sizeof high / sizeof high[0] ? high[i] : 0;
}
#else
/* XIP SSI BAUDR: even values only */
static uint32_t __not_in_flash_func(divisor_for)(const uint32_t clock_hz) {
    (void)clock_hz;
    uint32_t d = flash_setting;
    d += d & 1u;
    if (d < 2u) d = 2u;
    if (d > 254u) d = 254u;
    return d;
}

/* BAUDR can be written only while the SSI is disabled; the XIP setup of
 * boot2 (frame format, continuous read) stays as it is. */
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

bool clocks_set(const uint32_t cpu_mhz, const uint32_t flash, const int voltage) {
    uint vco, pd1, pd2;
    if (cpu_mhz == 0 || flash == 0 ||
        !check_sys_clock_khz(cpu_mhz * 1000u, &vco, &pd1, &pd2))
        return false;

    const uint32_t old_hz = clock_get_hz(clk_sys);
    const uint32_t new_hz = cpu_mhz * 1000000u;
#if PICO_RP2350
    /* a higher voltage before the clock change, a lower one after it */
    const enum vreg_voltage v = voltage_for(cpu_mhz, voltage);
    const bool v_up = voltage_mv(v) > voltage_mv(voltage_in_use);
    vreg_disable_voltage_limit();
    if (v_up) {
        vreg_set_voltage(v);
        voltage_in_use = v;
        sleep_ms(10);
    }
#else
    (void)voltage;
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    if (new_hz > old_hz) sleep_ms(33);
#endif

    flash_setting = flash;
    clocks_switch(old_hz, new_hz, vco, pd1, pd2);

#if PICO_RP2350
    if (!v_up && v != voltage_in_use) {
        vreg_set_voltage(v);
        voltage_in_use = v;
    }
#endif
    return true;
}

uint32_t clocks_flash_hz(void) {
    return flash_divisor ? clk_hz / flash_divisor : 0;
}

uint32_t clocks_voltage_mv(void) {
#if PICO_RP2350
    return voltage_in_use < 0 ? 0 : voltage_mv(voltage_in_use);
#else
    return flash_divisor ? 1300u : 0; /* VSEL at its highest */
#endif
}

void __no_inline_not_in_flash_func(clocks_flash_restore)(void) {
    if (flash_divisor) flash_timing_set(clk_hz, flash_divisor);
}
