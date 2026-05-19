#ifndef PERF_H
#define PERF_H

#include <stdint.h>

#define UART_STATUS_REG (*(volatile uint32_t *)0x02000000u)
#define UART_TX_REG     (*(volatile uint32_t *)0x02000004u)
#define UART_STATUS_TX_READY 1u

#define LED_REG (*(volatile uint32_t *)0x03000000u)

static inline void
uart_putc_raw(char c)
{
    while ((UART_STATUS_REG & UART_STATUS_TX_READY) == 0u) {
        __asm__ volatile ("nop");
    }

    UART_TX_REG = (uint32_t)(uint8_t)c;
}

static inline void
uart_putc(char c)
{
    if (c == '\n') {
        uart_putc_raw('\r');
    }

    uart_putc_raw(c);
}

static inline void
uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static inline void
perf_fence(void)
{
    __asm__ volatile ("fence" ::: "memory");
}

static inline uint32_t
read_mcycle_lo(void)
{
    uint32_t value;
    __asm__ volatile ("csrr %0, mcycle" : "=r" (value));
    return value;
}

static inline uint32_t
read_minstret_lo(void)
{
    uint32_t value;
    __asm__ volatile ("csrr %0, minstret" : "=r" (value));
    return value;
}

static inline void
enable_perf_counters(void)
{
    __asm__ volatile ("csrw mcountinhibit, x0" ::: "memory");
}

static inline uint32_t
mix32(uint32_t state, uint32_t value)
{
    state ^= value + 0x9e3779b9u + (state << 6) + (state >> 2);
    state ^= state >> 16;
    state *= 0x7feb352du;
    state ^= state >> 15;
    state *= 0x846ca68bu;
    state ^= state >> 16;
    return state;
}

static inline void
delay_cycles(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

#endif
