#include <stdint.h>

#include "ai_ops.h"
#include "perf.h"
#include "tiny_ai_model.h"

#define INPUT_DIM       TINY_AI_INPUT_DIM
#define HIDDEN_DIM      TINY_AI_HIDDEN_DIM
#define OUTPUT_DIM      TINY_AI_OUTPUT_DIM
#define SAMPLE_COUNT    TINY_AI_SAMPLE_COUNT

#define INPUT_WORDS     (INPUT_DIM / 4u)
#define HIDDEN_WORDS    (HIDDEN_DIM / 4u)

#define FC1_INT8_MACS_PER_SAMPLE (HIDDEN_DIM * INPUT_DIM)
#define FC2_INT8_MACS_PER_SAMPLE (OUTPUT_DIM * HIDDEN_DIM)
#define INT8_MACS_PER_SAMPLE \
    (FC1_INT8_MACS_PER_SAMPLE + FC2_INT8_MACS_PER_SAMPLE)
#define TOTAL_INT8_MACS (SAMPLE_COUNT * INT8_MACS_PER_SAMPLE)
#define TOTAL_DOT4_OPS  (TOTAL_INT8_MACS / 4u)

#define FC1_SHIFT       TINY_AI_FC1_SHIFT
#define FC2_SHIFT       TINY_AI_FC2_SHIFT
#define HIDDEN_CLAMP    TINY_AI_HIDDEN_CLAMP
#define OUTPUT_CLAMP    TINY_AI_OUTPUT_CLAMP

#define CLOCK_HZ 50000000u

#define LED_STATUS_INIT       0x21u
#define LED_STATUS_POWER_START 0x40u
#define LED_STATUS_BASELINE   0x31u
#define LED_STATUS_CUSTOM     0x32u
#define LED_STATUS_ALL_PASS   0xa5u
#define LED_STATUS_FAIL       0xefu

#define REPORT_DELAY_CYCLES 50000000u

#define POWER_SIM_VARIANT_BASELINE 0u
#define POWER_SIM_VARIANT_CUSTOM   1u

#define EXPECTED_TINY_AI_CHECKSUM TINY_AI_EXPECTED_CHECKSUM

typedef uint32_t (*run_fn_t)(void);

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
} run_result_t;

typedef struct {
    run_result_t baseline;
    run_result_t custom;
    uint32_t speedup_x100;
    uint32_t expected_matches;
    uint32_t pass;
} tiny_ai_result_t;

static uint16_t baseline_scores[SAMPLE_COUNT][OUTPUT_DIM];
static uint16_t custom_scores[SAMPLE_COUNT][OUTPUT_DIM];
static uint8_t baseline_classes[SAMPLE_COUNT];
static uint8_t custom_classes[SAMPLE_COUNT];

static volatile uint32_t sink;

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

static uint8_t argmax_u16(const uint16_t *scores)
{
    uint8_t best = 0;
    uint16_t best_score = scores[0];

    for (uint32_t i = 1; i < OUTPUT_DIM; ++i) {
        if (scores[i] > best_score) {
            best = (uint8_t)i;
            best_score = scores[i];
        }
    }

    return best;
}

static uint32_t mix_scores(uint32_t checksum, const uint16_t *scores,
    uint8_t predicted_class)
{
    checksum = mix32(checksum, predicted_class);
    for (uint32_t i = 0; i < OUTPUT_DIM; ++i) {
        checksum = mix32(checksum, scores[i] ^ (i * 0x45d9f3bu));
    }
    return checksum;
}

static void tiny_infer_baseline_one(const int8_t *input, uint16_t *scores)
{
    int8_t hidden[HIDDEN_DIM] __attribute__((aligned(4)));

    for (uint32_t h = 0; h < HIDDEN_DIM; ++h) {
        int32_t acc = tiny_ai_fc1_bias[h];

        for (uint32_t i = 0; i < INPUT_DIM; ++i) {
            acc += (int32_t)input[i] * (int32_t)tiny_ai_fc1_weight[h][i];
        }

        hidden[h] = (int8_t)clamp_baseline(acc >> FC1_SHIFT, HIDDEN_CLAMP);
    }

    for (uint32_t out = 0; out < OUTPUT_DIM; ++out) {
        int32_t acc = tiny_ai_fc2_bias[out];

        for (uint32_t h = 0; h < HIDDEN_DIM; ++h) {
            acc += (int32_t)hidden[h] *
                (int32_t)tiny_ai_fc2_weight[out][h];
        }

        scores[out] = (uint16_t)clamp_baseline(acc >> FC2_SHIFT, OUTPUT_CLAMP);
    }
}

static void tiny_fc_dot4_stream(const int8_t *activation,
    const int8_t *weight, uint32_t word_count, int32_t *acc)
{
    uintptr_t act_ptr = (uintptr_t)activation;
    uintptr_t weight_ptr = (uintptr_t)weight;
    uint32_t act_word;
    uint32_t weight_word;

    __asm__ volatile (
        ".option push\n"
        ".option norvc\n"
        ".balign 4\n"
        ".insn i 0x2b, 4, x14, %[count], 4\n"
        ".insn i 0x0b, 2, %[act_word], %[act_ptr], 4\n"
        ".insn i 0x0b, 2, %[weight_word], %[weight_ptr], 4\n"
        ".insn r 0x7b, 1, 0x54, %[acc], %[act_word], %[weight_word]\n"
        ".option pop\n"
        : [acc] "+r" (*acc),
          [act_ptr] "+r" (act_ptr),
          [weight_ptr] "+r" (weight_ptr),
          [act_word] "=&r" (act_word),
          [weight_word] "=&r" (weight_word)
        : [count] "r" (word_count)
        : "memory"
    );
}

static void tiny_infer_custom_one(const int8_t *input, uint16_t *scores)
{
    int8_t hidden[HIDDEN_DIM] __attribute__((aligned(4)));

    for (uint32_t h = 0; h < HIDDEN_DIM; ++h) {
        int32_t acc = tiny_ai_fc1_bias[h];

        tiny_fc_dot4_stream(input, tiny_ai_fc1_weight[h], INPUT_WORDS, &acc);
        hidden[h] = (int8_t)ai_clamp(acc >> FC1_SHIFT, HIDDEN_CLAMP);
    }

    for (uint32_t out = 0; out < OUTPUT_DIM; ++out) {
        int32_t acc = tiny_ai_fc2_bias[out];

        tiny_fc_dot4_stream(hidden, tiny_ai_fc2_weight[out],
            HIDDEN_WORDS, &acc);
        scores[out] = (uint16_t)ai_clamp(acc >> FC2_SHIFT, OUTPUT_CLAMP);
    }
}

__attribute__((noinline))
static uint32_t run_tiny_ai_baseline(void)
{
    uint32_t checksum = 0x54494e59u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        tiny_infer_baseline_one(tiny_ai_input_samples[sample],
            baseline_scores[sample]);
        baseline_classes[sample] = argmax_u16(baseline_scores[sample]);
        checksum = mix_scores(checksum, baseline_scores[sample],
            baseline_classes[sample]);
    }

    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t run_tiny_ai_custom(void)
{
    uint32_t checksum = 0x54494e59u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        tiny_infer_custom_one(tiny_ai_input_samples[sample],
            custom_scores[sample]);
        custom_classes[sample] = argmax_u16(custom_scores[sample]);
        checksum = mix_scores(checksum, custom_scores[sample],
            custom_classes[sample]);
    }

    sink = checksum;
    return checksum;
}

static run_result_t measure(run_fn_t fn)
{
    run_result_t result;

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

static uint32_t outputs_match(void)
{
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        if (baseline_classes[sample] != custom_classes[sample]) {
            return 0u;
        }

        for (uint32_t out = 0; out < OUTPUT_DIM; ++out) {
            if (baseline_scores[sample][out] != custom_scores[sample][out]) {
                return 0u;
            }
        }
    }

    return 1u;
}

static uint32_t expected_match_count(void)
{
    uint32_t matches = 0u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        if (baseline_classes[sample] ==
            (uint8_t)tiny_ai_expected_labels[sample]) {
            ++matches;
        }
    }

    return matches;
}

static void run_pair(tiny_ai_result_t *result)
{
    (void)run_tiny_ai_baseline();
    (void)run_tiny_ai_custom();

    LED_REG = LED_STATUS_BASELINE;
    result->baseline = measure(run_tiny_ai_baseline);

    LED_REG = LED_STATUS_CUSTOM;
    result->custom = measure(run_tiny_ai_custom);

    result->speedup_x100 =
        speedup_x100(result->baseline.cycles, result->custom.cycles);
    result->expected_matches = expected_match_count();
    result->pass = (result->baseline.checksum == result->custom.checksum) &&
        outputs_match() && (result->expected_matches == SAMPLE_COUNT);
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

static void uart_put_spaces(uint32_t count)
{
    while (count != 0u) {
        uart_putc(' ');
        --count;
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

static void uart_put_x100(uint32_t value_x100)
{
    uart_put_u32_dec(value_x100 / 100u);
    uart_putc('.');
    const uint32_t frac = value_x100 % 100u;
    uart_putc((char)('0' + (frac / 10u)));
    uart_putc((char)('0' + (frac % 10u)));
}

static uint32_t ratio_x100(uint32_t numerator, uint32_t denominator)
{
    if (denominator == 0u) {
        return 0u;
    }

    return (numerator * 100u + (denominator / 2u)) / denominator;
}

static void uart_print_classes(const uint8_t *classes)
{
    for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec(classes[i]);
    }
}

static void uart_print_expected_labels(void)
{
    for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec((uint32_t)tiny_ai_expected_labels[i]);
    }
}

static void uart_print_sample0_scores(const uint16_t *scores)
{
    for (uint32_t i = 0; i < OUTPUT_DIM; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec(scores[i]);
    }
}

static void uart_print_report(const tiny_ai_result_t *result)
{
    uart_puts("\nDE2i-150 CV32E40P tiny INT8 inference\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n");
    uart_puts("Model: ");
    uart_puts(TINY_AI_MODEL_NAME);
    uart_puts(" int8 64 -> 16 -> 4\n");
    uart_puts("Samples: ");
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nINT8 MACs: ");
    uart_put_u32_dec(TOTAL_INT8_MACS);
    uart_puts("\nCustom dot4 ops: ");
    uart_put_u32_dec(TOTAL_DOT4_OPS);
    uart_puts("\n\n");

    uart_puts("Variant    Cycles    Instret  Cyc/sample  Cyc/MAC  Checksum    Pass\n");
    uart_puts("--------  --------  --------  ----------  -------  ----------  ----\n");
    uart_puts("baseline  ");
    uart_put_u32_dec_right(result->baseline.cycles, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->baseline.instret, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->baseline.cycles / SAMPLE_COUNT, 10u);
    uart_puts("  ");
    uart_put_x100(ratio_x100(result->baseline.cycles, TOTAL_INT8_MACS));
    uart_puts("  ");
    uart_put_u32_hex8(result->baseline.checksum);
    uart_puts("  ");
    uart_puts(result->pass ? "yes" : "no");
    uart_putc('\n');

    uart_puts("custom    ");
    uart_put_u32_dec_right(result->custom.cycles, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->custom.instret, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->custom.cycles / SAMPLE_COUNT, 10u);
    uart_puts("  ");
    uart_put_x100(ratio_x100(result->custom.cycles, TOTAL_INT8_MACS));
    uart_puts("  ");
    uart_put_u32_hex8(result->custom.checksum);
    uart_puts("  ");
    uart_puts(result->pass ? "yes" : "no");
    uart_putc('\n');

    uart_puts("\nSpeed: ");
    uart_put_speed(result->speedup_x100);
    uart_puts("\nExpected labels:  ");
    uart_print_expected_labels();
    uart_puts("\nBaseline classes: ");
    uart_print_classes(baseline_classes);
    uart_puts("\nCustom classes:   ");
    uart_print_classes(custom_classes);
    uart_puts("\nAccuracy: ");
    uart_put_u32_dec(result->expected_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nSample0 scores baseline: ");
    uart_print_sample0_scores(baseline_scores[0]);
    uart_puts("\nSample0 scores custom:   ");
    uart_print_sample0_scores(custom_scores[0]);
    uart_puts("\n\n");
}

#ifdef POWER_SIM

#ifndef POWER_SIM_VARIANT
#define POWER_SIM_VARIANT POWER_SIM_VARIANT_CUSTOM
#endif

int main(void)
{
    uint32_t checksum;

    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();

    LED_REG = LED_STATUS_POWER_START;

#if POWER_SIM_VARIANT == POWER_SIM_VARIANT_BASELINE
    checksum = run_tiny_ai_baseline();
#elif POWER_SIM_VARIANT == POWER_SIM_VARIANT_CUSTOM
    checksum = run_tiny_ai_custom();
#else
#error "Unsupported POWER_SIM_VARIANT value"
#endif

    LED_REG = (checksum == EXPECTED_TINY_AI_CHECKSUM) ?
        LED_STATUS_ALL_PASS : LED_STATUS_FAIL;

    while (1) {
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}

#else

int main(void)
{
    tiny_ai_result_t result;

    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();

    LED_REG = LED_STATUS_POWER_START;
    run_pair(&result);

    LED_REG = result.pass ? LED_STATUS_ALL_PASS : LED_STATUS_FAIL;

    while (1) {
        uart_print_report(&result);
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}

#endif
