#ifndef PERF_H
#define PERF_H

#include <stdint.h>

#define UART_STATUS_REG (*(volatile uint32_t *)0x02000000u)
#define UART_TX_REG     (*(volatile uint32_t *)0x02000004u)
#define UART_RX_REG     (*(volatile uint32_t *)0x02000008u)
#define UART_STATUS_TX_READY       (1u << 0)
#define UART_STATUS_RX_VALID       (1u << 1)
#define UART_STATUS_RX_OVERRUN     (1u << 2)
#define UART_STATUS_RX_FRAME_ERROR (1u << 3)
#define UART_STATUS_RX_ERROR_MASK \
    (UART_STATUS_RX_OVERRUN | UART_STATUS_RX_FRAME_ERROR)

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

static inline uint32_t
uart_get_status(void)
{
    return UART_STATUS_REG;
}

static inline uint32_t
uart_rx_error_status(void)
{
    return UART_STATUS_REG & UART_STATUS_RX_ERROR_MASK;
}

static inline void
uart_clear_rx_errors(void)
{
    UART_STATUS_REG = UART_STATUS_RX_ERROR_MASK;
}

static inline uint32_t
uart_getc_nonblock(uint8_t *out)
{
    if ((UART_STATUS_REG & UART_STATUS_RX_VALID) == 0u) {
        return 0u;
    }

    *out = (uint8_t)UART_RX_REG;
    return 1u;
}

static inline uint8_t
uart_getc_blocking(void)
{
    uint8_t value;

    while (!uart_getc_nonblock(&value)) {
        __asm__ volatile ("nop");
    }

    return value;
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
