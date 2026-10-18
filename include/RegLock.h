#ifndef _REGLOCK_H
#define _REGLOCK_H

#if defined(__aarch64__)
/* m68k state lives in these vector registers, see include/M68k.h */
register __uint128_t reserved_reg_q19 asm("q19");
register __uint128_t reserved_reg_q20 asm("q20");
register __uint128_t reserved_reg_q21 asm("q21");
#endif

#endif /* _REGLOCK_H */
