/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Host hints for busy-wait loops.

    cpu_spin_prepare()  before a wait loop (AArch64 sevl)
    cpu_spin_wait()     inside the loop (AArch64 wfe, RISC-V pause)
    cpu_halt()          stop until the next interrupt (wfe / wfi)
*/

#ifndef _CPU_RELAX_H
#define _CPU_RELAX_H

#if defined(__aarch64__)

static inline void cpu_spin_prepare(void) { __asm__ __volatile__("sevl"); }
static inline void cpu_spin_wait(void) { __asm__ __volatile__("wfe"); }
static inline void cpu_halt(void) { __asm__ __volatile__("wfe"); }

#elif defined(__riscv)

static inline void cpu_spin_prepare(void) { }
/* pause (Zihintpause) is encoded as fence w,0, a no-op on cores without it */
static inline void cpu_spin_wait(void) { __asm__ __volatile__(".insn i 0x0f, 0, x0, x0, 0x010"); }
static inline void cpu_halt(void) { __asm__ __volatile__("wfi"); }

#else
#error "Unsupported host architecture"
#endif

#endif /* _CPU_RELAX_H */
