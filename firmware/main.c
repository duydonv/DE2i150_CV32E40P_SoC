#include <stdint.h>

#include "ai_ops.h"

#define LED_REG (*(volatile uint32_t *)0x03000000u)

static volatile uint32_t plw_data[4] __attribute__((aligned(4))) = {
    0x11223344u,
    0x55667788u,
    0x99aabbccu,
    0xdeadbeefu
};

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

static uint32_t pack_i8(int8_t b0, int8_t b1, int8_t b2, int8_t b3)
{
    return ((uint32_t)(uint8_t)b0 <<  0) |
           ((uint32_t)(uint8_t)b1 <<  8) |
           ((uint32_t)(uint8_t)b2 << 16) |
           ((uint32_t)(uint8_t)b3 << 24);
}

static int test_mac(void)
{
    int32_t x = ai_mac(7, -3, 5);
    int32_t y = ai_mac(-20, -7, -6);

    x = ai_mac(x, -4, 5);

    return x == -28 &&
           y == 22 &&
           ai_mac(0, 0, 1234) == 0;
}

static int test_dot4_acc(void)
{
    const uint32_t a = pack_i8(1, -2, 3, -4);
    const uint32_t b = pack_i8(5, 6, -7, -8);
    const uint32_t c = pack_i8(-1, -1, -1, -1);
    const uint32_t d = pack_i8(1, 2, 3, 4);
    const uint32_t e = pack_i8(127, -128, 1, -1);
    const uint32_t f = pack_i8(1, 1, -2, 3);

    return ai_dot4_acc(10, a, b) == 14 &&
           ai_dot4_acc(0, c, d) == -10 &&
           ai_dot4_acc(3, e, f) == -3;
}

static int test_relu(void)
{
    return ai_relu(-123) == 0 &&
           ai_relu(456) == 456 &&
           ai_relu(0) == 0 &&
           ai_relu(-2147483647 - 1) == 0;
}

static int test_clamp(void)
{
    return ai_clamp(-5, 300) == 0 &&
           ai_clamp(-2147483647 - 1, 300) == 0 &&
           ai_clamp(0, 0) == 0 &&
           ai_clamp(1, 0) == 0 &&
           ai_clamp(42, 300) == 42 &&
           ai_clamp(300, 300) == 300 &&
           ai_clamp(350, 300) == 300;
}

static int test_plw(void)
{
    volatile uint32_t *p = plw_data;
    const uint32_t a = AI_PLW_U32(p, 4);
    const uint32_t b = AI_PLW_U32(p, 4);
    const int forward_ok = (a == plw_data[0]) &&
                           (b == plw_data[1]) &&
                           (p == &plw_data[2]);

    p = &plw_data[2];
    const uint32_t c = AI_PLW_U32(p, -4);
    const uint32_t d = AI_PLW_U32(p, 8);

    return forward_ok &&
           c == plw_data[2] &&
           d == plw_data[1] &&
           p == &plw_data[3];
}

static int test_lp_setup(void)
{
    const uint32_t count = 5;
    uint32_t acc;

    __asm__ volatile (
        ".option push\n"
        ".option norvc\n"
        ".balign 4\n"
        "addi %[acc], x0, 0\n"
        ".insn i 0x2b, 4, x14, %[count], 4\n"
        "addi %[acc], %[acc], 1\n"
        "addi %[acc], %[acc], 2\n"
        "addi %[acc], %[acc], 3\n"
        ".option pop\n"
        : [acc] "=&r" (acc)
        : [count] "r" (count)
        : "memory"
    );

    if (acc != 30) {
        return 0;
    }

    __asm__ volatile (
        ".option push\n"
        ".option norvc\n"
        ".balign 4\n"
        "addi %[acc], x0, 1\n"
        ".insn i 0x2b, 4, x14, %[count], 4\n"
        "slli %[acc], %[acc], 1\n"
        "addi %[acc], %[acc], 1\n"
        "addi %[acc], %[acc], 0\n"
        ".option pop\n"
        : [acc] "=&r" (acc)
        : [count] "r" (4u)
        : "memory"
    );

    return acc == 31;
}

static uint32_t run_smoke_tests(void)
{
    if (!test_mac())      return 0xe1u;
    if (!test_dot4_acc()) return 0xe2u;
    if (!test_relu())     return 0xe3u;
    if (!test_clamp())    return 0xe4u;
    if (!test_plw())      return 0xe5u;
    if (!test_lp_setup()) return 0xe6u;

    return 0xa5u;
}

int main(void)
{
    const uint32_t status = run_smoke_tests();

    while (1) {
        LED_REG = status;
        delay(6000000u);
        LED_REG = 0x00u;
        delay(2000000u);
    }
}
