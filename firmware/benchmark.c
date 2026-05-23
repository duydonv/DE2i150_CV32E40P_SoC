#include <stdint.h>

#include "ai_ops.h"
#include "perf.h"

#define MAC_DATA_WORDS 64u
#define MAC_OUTPUTS    512u
#define MAC_TAPS       32u
#define MAC_CLAMP_MAX  2047u

#define DOT_DATA_WORDS 256u
#define DOT_OUTPUTS    128u
#define DOT_GROUPS     64u
#define DOT_CLAMP_MAX  255u

#define CLOCK_HZ 50000000u

#define BENCH_MAC_CLAMP_ID        1u
#define BENCH_DOT4_CLAMP_ID       2u
#define BENCH_DOT4_STREAM_ID      3u

#define LED_STATUS_INIT       0x01u
#define LED_STATUS_MAC_RUN    0x11u
#define LED_STATUS_DOT4_RUN   0x12u
#define LED_STATUS_STREAM_RUN 0x13u
#define LED_STATUS_ALL_PASS   0xa5u
#define LED_STATUS_FAIL_BASE  0xe0u

#define LED_STATUS_POWER_START 0x40u
#define LED_STATUS_POWER_FAIL  0xefu

#define REPORT_DELAY_CYCLES 50000000u

#define POWER_SIM_VARIANT_BASELINE 0u
#define POWER_SIM_VARIANT_CUSTOM   1u

#define EXPECTED_MAC_CLAMP_CHECKSUM  0x06535320u
#define EXPECTED_DOT4_CLAMP_CHECKSUM 0x9587070eu

typedef uint32_t (*bench_fn_t)(void);

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
} bench_run_t;

typedef struct {
    uint8_t id;
    uint32_t op_count;
    bench_run_t baseline;
    bench_run_t custom;
    uint32_t speedup_x100;
    uint32_t pass;
} bench_result_t;

static int32_t mac_lhs[MAC_DATA_WORDS];
static int32_t mac_rhs[MAC_DATA_WORDS];
static uint32_t dot_act[DOT_DATA_WORDS];
static uint32_t dot_weight[DOT_DATA_WORDS];

static volatile uint32_t sink;

static uint32_t pack_i8(int32_t b0, int32_t b1, int32_t b2, int32_t b3)
{
    return ((uint32_t)(uint8_t)b0 <<  0) |
           ((uint32_t)(uint8_t)b1 <<  8) |
           ((uint32_t)(uint8_t)b2 << 16) |
           ((uint32_t)(uint8_t)b3 << 24);
}

static int32_t wrap_i8(uint32_t value, uint32_t mul, uint32_t add)
{
    return (int32_t)(((value * mul + add) & 0x7fu) - 64u);
}

static void init_inputs(void)
{
    for (uint32_t i = 0; i < MAC_DATA_WORDS; ++i) {
        mac_lhs[i] = wrap_i8(i, 13u, 5u);
        mac_rhs[i] = wrap_i8(i, 17u, 9u);
    }

    for (uint32_t i = 0; i < DOT_DATA_WORDS; ++i) {
        dot_act[i] = pack_i8(
            wrap_i8(i, 13u,  5u),
            wrap_i8(i, 17u,  9u),
            wrap_i8(i, 19u,  3u),
            wrap_i8(i, 23u, 11u));
        dot_weight[i] = pack_i8(
            wrap_i8(i, 29u,  7u),
            wrap_i8(i, 31u,  1u),
            wrap_i8(i, 37u, 13u),
            wrap_i8(i, 41u, 15u));
    }
}

static uint32_t clamp_baseline(int32_t value, uint32_t upper_bound)
{
    if (value < 0) {
        return 0;
    }

    if ((uint32_t)value > upper_bound) {
        return upper_bound;
    }

    return (uint32_t)value;
}

__attribute__((noinline))
static uint32_t bench_mac_clamp_baseline(void)
{
    uint32_t checksum = 0x31415926u;

    for (uint32_t out = 0; out < MAC_OUTPUTS; ++out) {
        int32_t acc = (int32_t)(out & 15u) - 8;

        for (uint32_t tap = 0; tap < MAC_TAPS; ++tap) {
            const uint32_t lhs_idx = (out + tap * 3u) & (MAC_DATA_WORDS - 1u);
            const uint32_t rhs_idx = (out * 5u + tap) & (MAC_DATA_WORDS - 1u);
            acc += mac_lhs[lhs_idx] * mac_rhs[rhs_idx];
        }

        const uint32_t clamped = clamp_baseline(acc, MAC_CLAMP_MAX);
        checksum = mix32(checksum, clamped ^ (out * 0x45d9f3bu));
    }

    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t bench_mac_clamp_custom(void)
{
    uint32_t checksum = 0x31415926u;

    for (uint32_t out = 0; out < MAC_OUTPUTS; ++out) {
        int32_t acc = (int32_t)(out & 15u) - 8;

        for (uint32_t tap = 0; tap < MAC_TAPS; ++tap) {
            const uint32_t lhs_idx = (out + tap * 3u) & (MAC_DATA_WORDS - 1u);
            const uint32_t rhs_idx = (out * 5u + tap) & (MAC_DATA_WORDS - 1u);
            acc = ai_mac(acc, mac_lhs[lhs_idx], mac_rhs[rhs_idx]);
        }

        const uint32_t clamped = (uint32_t)ai_clamp(acc, MAC_CLAMP_MAX);
        checksum = mix32(checksum, clamped ^ (out * 0x45d9f3bu));
    }

    sink = checksum;
    return checksum;
}

static int32_t dot4_scalar_step(uint32_t activation, uint32_t weight)
{
    int32_t dot = 0;

    for (uint32_t lane = 0; lane < 4u; ++lane) {
        const int8_t a = (int8_t)(activation >> (lane * 8u));
        const int8_t w = (int8_t)(weight >> (lane * 8u));
        dot += (int32_t)a * (int32_t)w;
    }

    return dot;
}

__attribute__((noinline))
static uint32_t bench_dot4_clamp_baseline(void)
{
    uint32_t checksum = 0x27182818u;

    for (uint32_t out = 0; out < DOT_OUTPUTS; ++out) {
        const uint32_t base = ((out * 37u) & 3u) * DOT_GROUPS;
        int32_t acc = 0;

        for (uint32_t i = 0; i < DOT_GROUPS; ++i) {
            acc += dot4_scalar_step(dot_act[base + i], dot_weight[base + i]);
        }

        const uint32_t clamped = clamp_baseline(acc, DOT_CLAMP_MAX);
        checksum = mix32(checksum, clamped ^ (out * 0x119de1f3u));
    }

    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t bench_dot4_clamp_custom(void)
{
    uint32_t checksum = 0x27182818u;

    for (uint32_t out = 0; out < DOT_OUTPUTS; ++out) {
        const uint32_t base = ((out * 37u) & 3u) * DOT_GROUPS;
        int32_t acc = 0;

        for (uint32_t i = 0; i < DOT_GROUPS; ++i) {
            acc = ai_dot4_acc(acc, dot_act[base + i], dot_weight[base + i]);
        }

        const uint32_t clamped = (uint32_t)ai_clamp(acc, DOT_CLAMP_MAX);
        checksum = mix32(checksum, clamped ^ (out * 0x119de1f3u));
    }

    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t bench_dot4_stream_custom(void)
{
    uint32_t checksum = 0x27182818u;

    for (uint32_t out = 0; out < DOT_OUTPUTS; ++out) {
        const uint32_t base = ((out * 37u) & 3u) * DOT_GROUPS;
        uintptr_t act_ptr = (uintptr_t)&dot_act[base];
        uintptr_t weight_ptr = (uintptr_t)&dot_weight[base];
        uint32_t act_word;
        uint32_t weight_word;
        int32_t acc = 0;
        const uint32_t count = DOT_GROUPS;

        __asm__ volatile (
            ".option push\n"
            ".option norvc\n"
            ".balign 4\n"
            ".insn i 0x2b, 4, x14, %[count], 4\n"
            ".insn i 0x0b, 2, %[act_word], %[act_ptr], 4\n"
            ".insn i 0x0b, 2, %[weight_word], %[weight_ptr], 4\n"
            ".insn r 0x7b, 1, 0x54, %[acc], %[act_word], %[weight_word]\n"
            ".option pop\n"
            : [acc] "+r" (acc),
              [act_ptr] "+r" (act_ptr),
              [weight_ptr] "+r" (weight_ptr),
              [act_word] "=&r" (act_word),
              [weight_word] "=&r" (weight_word)
            : [count] "r" (count)
            : "memory"
        );

        const uint32_t clamped = (uint32_t)ai_clamp(acc, DOT_CLAMP_MAX);
        checksum = mix32(checksum, clamped ^ (out * 0x119de1f3u));
    }

    sink = checksum;
    return checksum;
}

static bench_run_t measure(bench_fn_t fn)
{
    bench_run_t result;

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = fn();

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;

    return result;
}

static uint32_t speedup_x100(uint32_t baseline_cycles, uint32_t custom_cycles)
{
    if (custom_cycles == 0u) {
        return 0u;
    }

    return (baseline_cycles * 100u + (custom_cycles / 2u)) / custom_cycles;
}

static void run_pair(bench_result_t *result,
    uint8_t id,
    uint32_t op_count,
    bench_fn_t baseline,
    bench_fn_t custom)
{
    (void)baseline();
    (void)custom();

    result->id = id;
    result->op_count = op_count;
    result->baseline = measure(baseline);
    result->custom = measure(custom);
    result->speedup_x100 =
        speedup_x100(result->baseline.cycles, result->custom.cycles);
    result->pass = result->baseline.checksum == result->custom.checksum;
}

static void uart_put_u32_dec(uint32_t value)
{
    char buf[10];
    uint32_t pos = 0;

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

static uint32_t uart_dec_digits(uint32_t value)
{
    uint32_t digits = 1u;

    while (value >= 10u) {
        value /= 10u;
        ++digits;
    }

    return digits;
}

static uint32_t uart_str_len(const char *s)
{
    uint32_t len = 0u;

    while (s[len] != '\0') {
        ++len;
    }

    return len;
}

static void uart_put_spaces(uint32_t count)
{
    while (count != 0u) {
        uart_putc(' ');
        --count;
    }
}

static void uart_put_str_left(const char *s, uint32_t width)
{
    const uint32_t len = uart_str_len(s);

    uart_puts(s);
    if (width > len) {
        uart_put_spaces(width - len);
    }
}

static void uart_put_u32_dec_right(uint32_t value, uint32_t width)
{
    const uint32_t len = uart_dec_digits(value);

    if (width > len) {
        uart_put_spaces(width - len);
    }
    uart_put_u32_dec(value);
}

static void uart_put_u32_hex8(uint32_t value)
{
    static const char hex[] = "0123456789abcdef";

    uart_puts("0x");
    for (uint32_t nibble = 0; nibble < 8u; ++nibble) {
        const uint32_t shift = 28u - (nibble * 4u);
        uart_putc(hex[(value >> shift) & 0xfu]);
    }
}

static void uart_put_speed(uint32_t speedup_x100)
{
    uart_put_u32_dec(speedup_x100 / 100u);
    uart_putc('.');
    const uint32_t frac = speedup_x100 % 100u;
    uart_putc((char)('0' + (frac / 10u)));
    uart_putc((char)('0' + (frac % 10u)));
    uart_putc('x');
}

static void uart_put_speed_right(uint32_t speedup_x100, uint32_t width)
{
    const uint32_t len = uart_dec_digits(speedup_x100 / 100u) + 4u;

    if (width > len) {
        uart_put_spaces(width - len);
    }
    uart_put_speed(speedup_x100);
}

static void uart_put_checksum_field(const bench_result_t *result,
    uint32_t width)
{
    const uint32_t len = result->pass ? 10u : 21u;

    if (width > len) {
        uart_put_spaces(width - len);
    }

    uart_put_u32_hex8(result->baseline.checksum);
    if (!result->pass) {
        uart_putc('/');
        uart_put_u32_hex8(result->custom.checksum);
    }
}

static const char *bench_name(uint8_t id)
{
    if (id == BENCH_MAC_CLAMP_ID) {
        return "mac_clamp";
    }

    if (id == BENCH_DOT4_CLAMP_ID) {
        return "dot4_acc_clamp";
    }

    if (id == BENCH_DOT4_STREAM_ID) {
        return "dot4_plw_lp_clamp";
    }

    return "unknown";
}

static void uart_print_result(const bench_result_t *result)
{
    uart_put_u32_dec_right(result->id, 2u);
    uart_puts("  ");
    uart_put_str_left(bench_name(result->id), 24u);
    uart_puts("  ");
    uart_put_str_left(result->pass ? "yes" : "no", 4u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->op_count, 6u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->baseline.cycles, 10u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->custom.cycles, 10u);
    uart_puts("  ");
    uart_put_speed_right(result->speedup_x100, 7u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->baseline.instret, 10u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->custom.instret, 10u);
    uart_puts("  ");
    uart_put_checksum_field(result, 21u);
    uart_putc('\n');
}

static void uart_print_report(const bench_result_t *results, uint32_t count)
{
    uart_puts("\nDE2i-150 CV32E40P benchmark\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n\n");
    uart_puts("ID  Benchmark                 Pass     Ops    Base cyc    Cust cyc    Speed    Base ins    Cust ins  Checksum\n");
    uart_puts("--  ------------------------  ----  ------  ----------  ----------  -------  ----------  ----------  ---------------------\n");

    for (uint32_t i = 0; i < count; ++i) {
        uart_print_result(&results[i]);
    }

    uart_puts("\nSpeed = baseline_cycles / custom_cycles\n");
}

#ifdef POWER_SIM

#ifndef POWER_SIM_BENCH
#define POWER_SIM_BENCH BENCH_MAC_CLAMP_ID
#endif

#ifndef POWER_SIM_VARIANT
#define POWER_SIM_VARIANT POWER_SIM_VARIANT_CUSTOM
#endif

int main(void)
{
    uint32_t checksum;
    uint32_t expected_checksum;

    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();
    init_inputs();

    LED_REG = LED_STATUS_POWER_START;

#if POWER_SIM_BENCH == BENCH_MAC_CLAMP_ID
    expected_checksum = EXPECTED_MAC_CLAMP_CHECKSUM;
#if POWER_SIM_VARIANT == POWER_SIM_VARIANT_BASELINE
    checksum = bench_mac_clamp_baseline();
#else
    checksum = bench_mac_clamp_custom();
#endif
#elif POWER_SIM_BENCH == BENCH_DOT4_CLAMP_ID
    expected_checksum = EXPECTED_DOT4_CLAMP_CHECKSUM;
#if POWER_SIM_VARIANT == POWER_SIM_VARIANT_BASELINE
    checksum = bench_dot4_clamp_baseline();
#else
    checksum = bench_dot4_clamp_custom();
#endif
#elif POWER_SIM_BENCH == BENCH_DOT4_STREAM_ID
    expected_checksum = EXPECTED_DOT4_CLAMP_CHECKSUM;
#if POWER_SIM_VARIANT == POWER_SIM_VARIANT_BASELINE
    checksum = bench_dot4_clamp_baseline();
#else
    checksum = bench_dot4_stream_custom();
#endif
#else
#error "Unsupported POWER_SIM_BENCH value"
#endif

    LED_REG = (checksum == expected_checksum) ? LED_STATUS_ALL_PASS :
        LED_STATUS_POWER_FAIL;

    while (1) {
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}

#else

int main(void)
{
    bench_result_t results[3];

    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();
    init_inputs();

    LED_REG = LED_STATUS_MAC_RUN;
    run_pair(&results[0],
        BENCH_MAC_CLAMP_ID,
        MAC_OUTPUTS * MAC_TAPS,
        bench_mac_clamp_baseline,
        bench_mac_clamp_custom);

    LED_REG = LED_STATUS_DOT4_RUN;
    run_pair(&results[1],
        BENCH_DOT4_CLAMP_ID,
        DOT_OUTPUTS * DOT_GROUPS,
        bench_dot4_clamp_baseline,
        bench_dot4_clamp_custom);

    LED_REG = LED_STATUS_STREAM_RUN;
    run_pair(&results[2],
        BENCH_DOT4_STREAM_ID,
        DOT_OUTPUTS * DOT_GROUPS,
        bench_dot4_clamp_baseline,
        bench_dot4_stream_custom);

    uint32_t fail_mask = 0u;
    for (uint32_t i = 0; i < 3u; ++i) {
        if (!results[i].pass) {
            fail_mask |= 1u << i;
        }
    }
    LED_REG = (fail_mask == 0u) ? LED_STATUS_ALL_PASS :
        (LED_STATUS_FAIL_BASE | fail_mask);

    while (1) {
        uart_print_report(results, 3u);
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}

#endif
