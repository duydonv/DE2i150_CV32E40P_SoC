#include <stdint.h>

#include "ai_ops.h"
#include "perf.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tflm_tiny_ai_model_data.h"
#include "tiny_ai_model.h"

#define CLOCK_HZ 50000000u
#define TFLM_TINY_TENSOR_ARENA_SIZE 8192u

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

#define FC1_OUTPUT_MULTIPLIER 1073741824
#define FC1_OUTPUT_SHIFT      (-8)
#define FC2_OUTPUT_MULTIPLIER 1073741824
#define FC2_OUTPUT_SHIFT      (-6)

#define FC1_ACTIVATION_MAX    127

#define LED_STATUS_INIT       0x61u
#define LED_STATUS_SETUP      0x62u
#define LED_STATUS_REF        0x63u
#define LED_STATUS_OPT        0x64u
#define LED_STATUS_ALL_PASS   0xa5u
#define LED_STATUS_FAIL       0xefu

#define REPORT_DELAY_CYCLES 50000000u

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
    uint32_t expected_matches;
    uint32_t pass;
    uint32_t status;
} run_result_t;

typedef struct {
    run_result_t ref;
    run_result_t opt;
    uint32_t speedup_x100;
    uint32_t class_mismatches;
    uint32_t score_mismatches;
    uint32_t pass;
} tflm_tiny_result_t;

static uint8_t tflm_scores[SAMPLE_COUNT][OUTPUT_DIM];
static uint8_t opt_scores[SAMPLE_COUNT][OUTPUT_DIM];
static uint8_t tflm_classes[SAMPLE_COUNT];
static uint8_t opt_classes[SAMPLE_COUNT];
static uint8_t tensor_arena[TFLM_TINY_TENSOR_ARENA_SIZE]
    __attribute__((aligned(16)));

static tflite::MicroInterpreter *g_interpreter;
static TfLiteTensor *g_input;
static TfLiteTensor *g_output;

static volatile uint32_t sink;
static uint32_t run_status;

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

static void uart_put_x100(uint32_t value_x100)
{
    uart_put_u32_dec(value_x100 / 100u);
    uart_putc('.');
    const uint32_t frac = value_x100 % 100u;
    uart_putc((char)('0' + (frac / 10u)));
    uart_putc((char)('0' + (frac % 10u)));
}

static void uart_put_speed(uint32_t speedup_x100)
{
    uart_put_x100(speedup_x100);
    uart_putc('x');
}

static uint32_t ratio_x100(uint32_t numerator, uint32_t denominator)
{
    if (denominator == 0u) {
        return 0u;
    }

    return (numerator * 100u + (denominator / 2u)) / denominator;
}

static uint8_t argmax_u8(const uint8_t *scores)
{
    uint8_t best = 0u;
    uint8_t best_score = scores[0];

    for (uint32_t i = 1; i < OUTPUT_DIM; ++i) {
        if (scores[i] > best_score) {
            best = (uint8_t)i;
            best_score = scores[i];
        }
    }

    return best;
}

static uint32_t mix_scores(uint32_t checksum, const uint8_t *scores,
    uint8_t predicted_class)
{
    checksum = mix32(checksum, predicted_class);
    for (uint32_t i = 0; i < OUTPUT_DIM; ++i) {
        checksum = mix32(checksum, scores[i] ^ (i * 0x45d9f3bu));
    }
    return checksum;
}

static uint8_t output_to_score(int8_t value)
{
    return (uint8_t)((int32_t)value + 128);
}

static int8_t requant_relu_i8_like_tflm(int32_t acc, int32_t multiplier,
    int shift, uint32_t activation_max)
{
    int32_t value =
        tflite::MultiplyByQuantizedMultiplier(acc, multiplier, shift);
    value = ai_clamp(value, activation_max);
    return (int8_t)value;
}

static uint8_t requant_output_score_like_tflm(int32_t acc)
{
    int32_t value = tflite::MultiplyByQuantizedMultiplier(
        acc, FC2_OUTPUT_MULTIPLIER, FC2_OUTPUT_SHIFT);
    value = ai_clamp(value, TINY_AI_OUTPUT_CLAMP);
    return (uint8_t)value;
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

static void tiny_infer_opt_one(const int8_t *input, uint8_t *scores)
{
    int8_t hidden[HIDDEN_DIM] __attribute__((aligned(4)));

    for (uint32_t h = 0; h < HIDDEN_DIM; ++h) {
        int32_t acc = tiny_ai_fc1_bias[h];

        tiny_fc_dot4_stream(input, tiny_ai_fc1_weight[h], INPUT_WORDS, &acc);
        hidden[h] = requant_relu_i8_like_tflm(acc, FC1_OUTPUT_MULTIPLIER,
            FC1_OUTPUT_SHIFT, FC1_ACTIVATION_MAX);
    }

    for (uint32_t out = 0; out < OUTPUT_DIM; ++out) {
        int32_t acc = tiny_ai_fc2_bias[out];

        tiny_fc_dot4_stream(hidden, tiny_ai_fc2_weight[out],
            HIDDEN_WORDS, &acc);
        scores[out] = requant_output_score_like_tflm(acc);
    }
}

__attribute__((noinline))
static uint32_t run_tflm_tiny_samples(void)
{
    uint32_t checksum = 0x54464d59u;
    run_status = 0u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        for (uint32_t i = 0; i < INPUT_DIM; ++i) {
            g_input->data.int8[i] = tiny_ai_input_samples[sample][i];
        }

        if (g_interpreter->Invoke() != kTfLiteOk) {
            run_status = 0xe006u;
            sink = run_status;
            return mix32(checksum, run_status);
        }

        for (uint32_t out = 0; out < OUTPUT_DIM; ++out) {
            tflm_scores[sample][out] =
                output_to_score(g_output->data.int8[out]);
        }
        tflm_classes[sample] = argmax_u8(tflm_scores[sample]);
        checksum = mix_scores(checksum, tflm_scores[sample],
            tflm_classes[sample]);
    }

    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t run_opt_tiny_samples(void)
{
    uint32_t checksum = 0x54464d59u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        tiny_infer_opt_one(tiny_ai_input_samples[sample],
            opt_scores[sample]);
        opt_classes[sample] = argmax_u8(opt_scores[sample]);
        checksum = mix_scores(checksum, opt_scores[sample],
            opt_classes[sample]);
    }

    sink = checksum;
    return checksum;
}

static uint32_t expected_match_count(const uint8_t *classes)
{
    uint32_t matches = 0u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        if (classes[sample] ==
            (uint8_t)tiny_ai_expected_labels[sample]) {
            ++matches;
        }
    }

    return matches;
}

static uint32_t class_mismatch_count(void)
{
    uint32_t mismatches = 0u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        if (tflm_classes[sample] != opt_classes[sample]) {
            ++mismatches;
        }
    }

    return mismatches;
}

static uint32_t score_mismatch_count(void)
{
    uint32_t mismatches = 0u;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
        for (uint32_t out = 0; out < OUTPUT_DIM; ++out) {
            if (tflm_scores[sample][out] != opt_scores[sample][out]) {
                ++mismatches;
            }
        }
    }

    return mismatches;
}

static run_result_t measure_tflm_reference(void)
{
    run_result_t result;

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_tflm_tiny_samples();

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.expected_matches = expected_match_count(tflm_classes);
    result.status = run_status;
    result.pass = (run_status == 0u) &&
        (result.expected_matches == SAMPLE_COUNT);

    return result;
}

static run_result_t measure_opt_tiny(void)
{
    run_result_t result;

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_opt_tiny_samples();

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.expected_matches = expected_match_count(opt_classes);
    result.status = 0u;
    result.pass = (result.expected_matches == SAMPLE_COUNT);

    return result;
}

static uint32_t speedup_x100(uint32_t ref_cycles, uint32_t opt_cycles)
{
    if (opt_cycles == 0u) {
        return 0u;
    }

    return (ref_cycles * 100u + (opt_cycles / 2u)) / opt_cycles;
}

static void measure_ref_vs_opt(tflm_tiny_result_t *result)
{
    (void)run_tflm_tiny_samples();
    (void)run_opt_tiny_samples();

    LED_REG = LED_STATUS_REF;
    result->ref = measure_tflm_reference();

    LED_REG = LED_STATUS_OPT;
    result->opt = measure_opt_tiny();

    result->speedup_x100 =
        speedup_x100(result->ref.cycles, result->opt.cycles);
    result->class_mismatches = class_mismatch_count();
    result->score_mismatches = score_mismatch_count();
    result->pass = result->ref.pass && result->opt.pass &&
        (result->ref.checksum == result->opt.checksum) &&
        (result->class_mismatches == 0u) &&
        (result->score_mismatches == 0u);
}

static uint32_t setup_tflm(tflite::MicroInterpreter *interpreter)
{
    g_interpreter = interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        return 0xe003u;
    }

    g_input = interpreter->input(0);
    g_output = interpreter->output(0);
    if (g_input == nullptr || g_output == nullptr) {
        return 0xe004u;
    }

    if (g_input->type != kTfLiteInt8 || g_output->type != kTfLiteInt8 ||
        g_input->bytes != INPUT_DIM || g_output->bytes != OUTPUT_DIM) {
        return 0xe005u;
    }

    (void)run_tflm_tiny_samples();
    if (run_status != 0u) {
        return run_status;
    }

    return 0u;
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

static void uart_print_sample0_scores(const uint8_t *scores)
{
    for (uint32_t i = 0; i < OUTPUT_DIM; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec(scores[i]);
    }
}

static void uart_print_variant_row(const char *name, const run_result_t *result,
    uint32_t pass)
{
    uart_puts(name);
    uart_puts("  ");
    uart_put_u32_dec_right(result->cycles, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->instret, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->cycles / SAMPLE_COUNT, 10u);
    uart_puts("  ");
    uart_put_x100(ratio_x100(result->cycles, TOTAL_INT8_MACS));
    uart_puts("  ");
    uart_put_u32_hex8(result->checksum);
    uart_puts("  ");
    uart_put_u32_hex8(result->status);
    uart_puts("  ");
    uart_puts(pass ? "yes" : "no");
    uart_putc('\n');
}

static void uart_print_report(const tflm_tiny_result_t *result)
{
    uart_puts("\nDE2i-150 CV32E40P TFLM tiny INT8 MLP ref-vs-opt\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n");
    uart_puts("Model: ");
    uart_puts(TINY_AI_MODEL_NAME);
    uart_puts(" int8 64 -> 16 -> 4\n");
    uart_puts("Model bytes: ");
    uart_put_u32_dec(g_tflm_tiny_ai_model_data_size);
    uart_puts("\nTensor arena: ");
    uart_put_u32_dec(TFLM_TINY_TENSOR_ARENA_SIZE);
    uart_puts(" bytes\nSamples: ");
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nINT8 MACs: ");
    uart_put_u32_dec(TOTAL_INT8_MACS);
    uart_puts("\nCustom dot4 ops: ");
    uart_put_u32_dec(TOTAL_DOT4_OPS);
    uart_puts("\nOptimized kernel: cv.sdotsp.b + cv.lw + cv.setup + cv.clipur");
    uart_puts("\n\n");

    uart_puts("Variant   Cycles    Instret  Cyc/sample  Cyc/MAC  Checksum    Status      Pass\n");
    uart_puts("--------  --------  --------  ----------  -------  ----------  ----------  ----\n");
    uart_print_variant_row("tflm_ref", &result->ref, result->ref.pass);
    uart_print_variant_row("pulp_opt", &result->opt,
        result->opt.pass && (result->score_mismatches == 0u) &&
        (result->class_mismatches == 0u));
    uart_puts("\n\nExpected labels: ");
    uart_print_expected_labels();
    uart_puts("\nTFLM classes:    ");
    uart_print_classes(tflm_classes);
    uart_puts("\nOpt classes:     ");
    uart_print_classes(opt_classes);
    uart_puts("\nRef accuracy: ");
    uart_put_u32_dec(result->ref.expected_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nOpt accuracy: ");
    uart_put_u32_dec(result->opt.expected_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nClass mismatches: ");
    uart_put_u32_dec(result->class_mismatches);
    uart_puts("\nScore mismatches: ");
    uart_put_u32_dec(result->score_mismatches);
    uart_puts("\nSpeedup: ");
    uart_put_speed(result->speedup_x100);
    uart_puts("\nOverall pass: ");
    uart_puts(result->pass ? "yes" : "no");
    uart_puts("\nSample0 scores TFLM: ");
    uart_print_sample0_scores(tflm_scores[0]);
    uart_puts("\nSample0 scores opt:  ");
    uart_print_sample0_scores(opt_scores[0]);
    uart_puts("\n\n");
}

int main(void)
{
    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();

    LED_REG = LED_STATUS_SETUP;
    tflite::InitializeTarget();

    const tflite::Model *model =
        tflite::GetModel(g_tflm_tiny_ai_model_data);
    tflm_tiny_result_t result = {};

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        result.ref.status = 0xe001u;
    } else {
        tflite::MicroMutableOpResolver<1> resolver;

        if (resolver.AddFullyConnected() != kTfLiteOk) {
            result.ref.status = 0xe002u;
        } else {
            tflite::MicroInterpreter interpreter(model, resolver,
                tensor_arena, sizeof(tensor_arena));

            result.ref.status = setup_tflm(&interpreter);
            if (result.ref.status == 0u) {
                measure_ref_vs_opt(&result);
            }
        }
    }

    LED_REG = result.pass ? LED_STATUS_ALL_PASS : LED_STATUS_FAIL;

    while (1) {
        uart_print_report(&result);
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}
