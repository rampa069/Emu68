/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Periodic supervisor timer, the RV64 counterpart of the generic timer use
    in src/aarch64. The deadline is set through the SBI TIME extension
    (EID 0x54494d45, which also clears a pending timer interrupt), so it
    works with or without Sstc. Each tick is counted and reported to the
    m68k side as an ARM interrupt (M68kReportInterrupt(1), level 6), the
    way an unclaimed IRQ on core 0 is reported on AArch64
    (src/aarch64/vectors.c); the m68k handler acknowledges it through
    MOVEC JITCTRL2, bit 29.

    Deadlines advance by whole periods from the first one, so ticks do not
    drift; a deadline already in the past is moved to now + period.
*/

#include <stdint.h>
#include "support.h"
#include "riscv64/timer.h"

void M68kReportInterrupt(int irq);

volatile uint64_t rv_timer_ticks;
static uint64_t period, next;

static void sbi_set_timer(uint64_t when)
{
    register long a0 __asm__("a0") = (long)when;
    register long a1 __asm__("a1");
    register long a6 __asm__("a6") = 0;
    register long a7 __asm__("a7") = 0x54494d45;    /* TIME */
    __asm__ volatile("ecall" : "+r"(a0), "=r"(a1) : "r"(a6), "r"(a7) : "memory");
}

static inline uint64_t now(void)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

int rv_timer_running(void)
{
    return period != 0;
}

void rv_timer_start(uint64_t timebase, uint32_t period_us)
{
    period = timebase * period_us / 1000000;
    if (period == 0)
        period = 1;
    next = now() + period;
    sbi_set_timer(next);
    __asm__ volatile("csrs sie, %0" :: "r"(1ul << 5));          /* STIE */
    __asm__ volatile("csrs sstatus, %0" :: "r"(1ul << 1));      /* SIE */
}

void rv_timer_interrupt(void)
{
    rv_timer_ticks++;
    next += period;
    uint64_t t = now();
    if (next <= t)
        next = t + period;
    sbi_set_timer(next);
    M68kReportInterrupt(1);
}
