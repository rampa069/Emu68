/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _EMU68_RISCV64_TIMER_H
#define _EMU68_RISCV64_TIMER_H

#include <stdint.h>

/* Periodic supervisor timer (src/riscv64/timer.c) */
void rv_timer_start(uint64_t timebase, uint32_t period_us);
void rv_timer_interrupt(void);      /* from trap_handler, scause = interrupt 5 */
int rv_timer_running(void);
extern volatile uint64_t rv_timer_ticks;

#endif /* _EMU68_RISCV64_TIMER_H */
