#ifndef AI_OPS_H
#define AI_OPS_H

#include <stdint.h>

/*
 * Firmware-level mapping from the gem5 prototype operations to the CORE-V
 * PULP instructions already implemented by CV32E40P when COREV_PULP=1.
 *
 * The stock riscv64-unknown-elf toolchain used for bring-up does not know the
 * CORE-V mnemonics, so these wrappers use GNU assembler .insn directives.
 */

static inline int32_t
ai_mac(int32_t acc, int32_t a, int32_t b)
{
    __asm__ volatile (
        ".insn r 0x2b, 3, 0x48, %0, %1, %2"
        : "+r" (acc)
        : "r" (a), "r" (b)
    );
    return acc;
}

static inline int32_t
ai_dot4_acc(int32_t acc, uint32_t a_packed, uint32_t b_packed)
{
    __asm__ volatile (
        ".insn r 0x7b, 1, 0x54, %0, %1, %2"
        : "+r" (acc)
        : "r" (a_packed), "r" (b_packed)
    );
    return acc;
}

static inline int32_t
ai_relu(int32_t value)
{
    int32_t out;
    __asm__ volatile (
        ".insn r 0x2b, 3, 0x2d, %0, %1, x0"
        : "=r" (out)
        : "r" (value)
    );
    return out;
}

static inline int32_t
ai_clamp(int32_t value, uint32_t upper_bound)
{
    int32_t out;
    __asm__ volatile (
        ".insn r 0x2b, 3, 0x3b, %0, %1, %2"
        : "=r" (out)
        : "r" (value), "r" (upper_bound)
    );
    return out;
}

/*
 * CORE-V cv.lw rd, (rs1), Imm:
 *   rd = Mem32(rs1)
 *   rs1 += Sext(Imm)
 *
 * This intentionally follows the hardware ISA, not the earlier gem5
 * prototype where the load address was rs1 + imm and the post-increment was
 * fixed to 4. For an offset stream, pre-bias the pointer once before the loop.
 */
#define AI_PLW_U32(ptr_lvalue, inc_bytes)                                      \
    ({                                                                         \
        uintptr_t _ai_p = (uintptr_t)(ptr_lvalue);                             \
        uint32_t _ai_v;                                                        \
        __asm__ volatile (                                                     \
            ".insn i 0x0b, 2, %0, %1, %2"                                      \
            : "=&r" (_ai_v), "+r" (_ai_p)                                     \
            : "i" (inc_bytes)                                                 \
            : "memory"                                                        \
        );                                                                     \
        (ptr_lvalue) = (void *)_ai_p;                                          \
        _ai_v;                                                                 \
    })

/*
 * CORE-V cv.setup L0, rs1, uimmL. body_bytes must be a compile-time multiple
 * of 4 and describes the loop body length in bytes, matching the assembler
 * mnemonic convention used in the upstream hwlp_test examples.
 *
 * The raw immediate field used by .insn is not byte-granular. The RTL forms
 * the internal end marker as pc + (raw_imm << 2), and the controller compares
 * end_marker == last_body_pc + 4. Therefore raw_imm is body_words + 1.
 */
#define AI_LP_SETUP_L0(count, body_bytes)                                      \
    __asm__ volatile (                                                         \
        ".insn i 0x2b, 4, x14, %0, %1"                                         \
        :                                                                      \
        : "r" (count), "i" (((body_bytes) / 4) + 1)                           \
        : "memory"                                                            \
    )

#endif
