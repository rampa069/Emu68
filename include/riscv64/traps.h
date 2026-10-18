/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _EMU68_RISCV64_TRAPS_H
#define _EMU68_RISCV64_TRAPS_H

#include <stdint.h>

/* Layout must match src/riscv64/trap_entry.S */
struct trap_frame {
    uint64_t x[32];     /* x[0] reads as 0, x[2] is sp before the trap */
    uint64_t sepc;
    uint64_t sstatus;
};

void trap_handler(struct trap_frame *f);

#endif /* _EMU68_RISCV64_TRAPS_H */
