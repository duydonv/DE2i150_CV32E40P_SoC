#include <stdint.h>

#include "perf.h"

#define CLOCK_HZ 50000000u

#define RX_SYNC0 0x55u
#define RX_SYNC1 0xaau
#define RX_CMD_ECHO 0x01u
#define RX_MAX_PAYLOAD 512u

#define LED_STATUS_INIT      0x70u
#define LED_STATUS_WAIT      0x71u
#define LED_STATUS_RECV      0x72u
#define LED_STATUS_PASS      0xa5u
#define LED_STATUS_FAIL      0xefu

static uint8_t payload[RX_MAX_PAYLOAD] __attribute__((aligned(4)));
static volatile uint32_t sink;

static void uart_put_u32_dec(uint32_t value)
{
    char buf[10];
    uint32_t pos = 0u;

    if (value == 0u) {
        uart_putc('0');
        return;
    }

    while (value != 0u) {
        buf[pos++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (pos != 0u) {
        uart_putc(buf[--pos]);
    }
}

static void uart_put_u32_hex8(uint32_t value)
{
    static const char hex[] = "0123456789abcdef";

    uart_puts("0x");
    for (uint32_t nibble = 0u; nibble < 8u; ++nibble) {
        const uint32_t shift = 28u - (nibble * 4u);
        uart_putc(hex[(value >> shift) & 0xfu]);
    }
}

static void uart_put_u8_hex2(uint8_t value)
{
    static const char hex[] = "0123456789abcdef";

    uart_puts("0x");
    uart_putc(hex[(value >> 4) & 0xfu]);
    uart_putc(hex[value & 0xfu]);
}

static uint32_t frame_checksum_update(uint32_t state, uint8_t value)
{
    state ^= value;
    state *= 16777619u;
    return state;
}

static uint32_t frame_checksum_begin(void)
{
    return 2166136261u;
}

static uint32_t frame_checksum_header(uint8_t cmd, uint16_t len)
{
    uint32_t checksum = frame_checksum_begin();

    checksum = frame_checksum_update(checksum, cmd);
    checksum = frame_checksum_update(checksum, (uint8_t)(len & 0xffu));
    checksum = frame_checksum_update(checksum, (uint8_t)(len >> 8));
    return checksum;
}

static uint32_t read_u32_le(void)
{
    uint32_t value = 0u;

    value |= (uint32_t)uart_getc_blocking() << 0;
    value |= (uint32_t)uart_getc_blocking() << 8;
    value |= (uint32_t)uart_getc_blocking() << 16;
    value |= (uint32_t)uart_getc_blocking() << 24;
    return value;
}

static void discard_bytes(uint16_t len)
{
    while (len != 0u) {
        sink = uart_getc_blocking();
        --len;
    }
}

static void wait_for_sync(void)
{
    uint8_t prev = 0u;

    while (1) {
        const uint8_t cur = uart_getc_blocking();
        if (prev == RX_SYNC0 && cur == RX_SYNC1) {
            return;
        }
        prev = cur;
    }
}

static void print_banner(void)
{
    uart_puts("\nDE2i-150 CV32E40P UART RX smoke\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n");
    uart_puts("Frame: 55 aa cmd len_lo len_hi payload checksum_le32\n");
    uart_puts("Checksum: FNV-1a over cmd,len_lo,len_hi,payload\n");
    uart_puts("Ready\n");
}

static void print_ok(uint32_t seq, uint8_t cmd, uint16_t len,
    uint32_t checksum, uint32_t cycles, uint32_t status)
{
    uart_puts("OK seq=");
    uart_put_u32_dec(seq);
    uart_puts(" cmd=");
    uart_put_u8_hex2(cmd);
    uart_puts(" len=");
    uart_put_u32_dec(len);
    uart_puts(" checksum=");
    uart_put_u32_hex8(checksum);
    uart_puts(" cycles=");
    uart_put_u32_dec(cycles);
    uart_puts(" status=");
    uart_put_u32_hex8(status);
    if (len != 0u) {
        uart_puts(" first=");
        uart_put_u8_hex2(payload[0]);
        uart_puts(" last=");
        uart_put_u8_hex2(payload[len - 1u]);
    }
    uart_putc('\n');
}

static void print_err(uint32_t seq, uint32_t code, uint32_t expected,
    uint32_t received, uint32_t status)
{
    uart_puts("ERR seq=");
    uart_put_u32_dec(seq);
    uart_puts(" code=");
    uart_put_u32_hex8(code);
    uart_puts(" expected=");
    uart_put_u32_hex8(expected);
    uart_puts(" received=");
    uart_put_u32_hex8(received);
    uart_puts(" status=");
    uart_put_u32_hex8(status);
    uart_putc('\n');
}

int main(void)
{
    uint32_t seq = 0u;

    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();
    uart_clear_rx_errors();
    print_banner();

    while (1) {
        LED_REG = LED_STATUS_WAIT;
        wait_for_sync();
        uart_clear_rx_errors();

        LED_REG = LED_STATUS_RECV;
        perf_fence();
        const uint32_t cycle_start = read_mcycle_lo();

        const uint8_t cmd = uart_getc_blocking();
        const uint16_t len_lo = uart_getc_blocking();
        const uint16_t len_hi = uart_getc_blocking();
        const uint16_t len = (uint16_t)(len_lo | (len_hi << 8));
        uint32_t checksum = frame_checksum_header(cmd, len);
        uint32_t expected_checksum = 0u;
        uint32_t received_checksum;
        uint32_t status;

        ++seq;

        if (cmd != RX_CMD_ECHO) {
            discard_bytes(len);
            received_checksum = read_u32_le();
            status = uart_get_status();
            LED_REG = LED_STATUS_FAIL;
            print_err(seq, 0xe001u, expected_checksum,
                received_checksum, status);
            continue;
        }

        if (len > RX_MAX_PAYLOAD) {
            discard_bytes(len);
            received_checksum = read_u32_le();
            status = uart_get_status();
            LED_REG = LED_STATUS_FAIL;
            print_err(seq, 0xe002u, expected_checksum,
                received_checksum, status);
            continue;
        }

        for (uint32_t i = 0u; i < len; ++i) {
            const uint8_t byte = uart_getc_blocking();
            payload[i] = byte;
            checksum = frame_checksum_update(checksum, byte);
        }
        expected_checksum = checksum;
        received_checksum = read_u32_le();

        perf_fence();
        const uint32_t cycle_end = read_mcycle_lo();
        const uint32_t cycles = cycle_end - cycle_start;
        status = uart_get_status();

        if (checksum == received_checksum &&
            (status & UART_STATUS_RX_ERROR_MASK) == 0u) {
            LED_REG = LED_STATUS_PASS;
            sink = checksum;
            print_ok(seq, cmd, len, checksum, cycles, status);
        } else {
            LED_REG = LED_STATUS_FAIL;
            sink = status;
            print_err(seq, 0xe003u, checksum, received_checksum, status);
        }
    }
}
