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

#define INPUT_WORDS     (INPUT_DIM / 4u)
#define HIDDEN_WORDS    (HIDDEN_DIM / 4u)

#define FC1_INT8_MACS_PER_SAMPLE (HIDDEN_DIM * INPUT_DIM)
#define FC2_INT8_MACS_PER_SAMPLE (OUTPUT_DIM * HIDDEN_DIM)
#define INT8_MACS_PER_SAMPLE \
    (FC1_INT8_MACS_PER_SAMPLE + FC2_INT8_MACS_PER_SAMPLE)

#define FC1_OUTPUT_MULTIPLIER 1073741824
#define FC1_OUTPUT_SHIFT      (-8)
#define FC2_OUTPUT_MULTIPLIER 1073741824
#define FC2_OUTPUT_SHIFT      (-6)

#define FC1_ACTIVATION_MAX    127

#define RX_SYNC0 0x55u
#define RX_SYNC1 0xaau
#define RX_CMD_PING  0x10u
#define RX_CMD_INFER 0x11u
#define RX_MAX_PAYLOAD INPUT_DIM

#define LED_STATUS_INIT       0x80u
#define LED_STATUS_SETUP      0x81u
#define LED_STATUS_WAIT       0x82u
#define LED_STATUS_RECV       0x83u
#define LED_STATUS_REF        0x84u
#define LED_STATUS_OPT        0x85u
#define LED_STATUS_PASS       0xa5u
#define LED_STATUS_FAIL       0xefu

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
    uint32_t status;
    uint8_t predicted_class;
    uint8_t pass;
} infer_result_t;

static uint8_t tensor_arena[TFLM_TINY_TENSOR_ARENA_SIZE]
    __attribute__((aligned(16)));
static uint8_t payload[RX_MAX_PAYLOAD] __attribute__((aligned(4)));
static int8_t runtime_input[INPUT_DIM] __attribute__((aligned(4)));
static uint8_t tflm_scores[OUTPUT_DIM];
static uint8_t opt_scores[OUTPUT_DIM];

static tflite::MicroInterpreter *g_interpreter;
static TfLiteTensor *g_input;
static TfLiteTensor *g_output;

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

static uint8_t argmax_u8(const uint8_t *scores)
{
    uint8_t best = 0u;
    uint8_t best_score = scores[0];

    for (uint32_t i = 1u; i < OUTPUT_DIM; ++i) {
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
    for (uint32_t i = 0u; i < OUTPUT_DIM; ++i) {
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

    for (uint32_t h = 0u; h < HIDDEN_DIM; ++h) {
        int32_t acc = tiny_ai_fc1_bias[h];

        tiny_fc_dot4_stream(input, tiny_ai_fc1_weight[h], INPUT_WORDS, &acc);
        hidden[h] = requant_relu_i8_like_tflm(acc, FC1_OUTPUT_MULTIPLIER,
            FC1_OUTPUT_SHIFT, FC1_ACTIVATION_MAX);
    }

    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        int32_t acc = tiny_ai_fc2_bias[out];

        tiny_fc_dot4_stream(hidden, tiny_ai_fc2_weight[out],
            HIDDEN_WORDS, &acc);
        scores[out] = requant_output_score_like_tflm(acc);
    }
}

__attribute__((noinline))
static uint32_t run_tflm_one(const int8_t *input, uint8_t *scores,
    uint32_t *status)
{
    uint32_t checksum = 0x54554652u;

    *status = 0u;
    for (uint32_t i = 0u; i < INPUT_DIM; ++i) {
        g_input->data.int8[i] = input[i];
    }

    if (g_interpreter->Invoke() != kTfLiteOk) {
        *status = 0xe006u;
        sink = *status;
        return mix32(checksum, *status);
    }

    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        scores[out] = output_to_score(g_output->data.int8[out]);
    }

    const uint8_t predicted_class = argmax_u8(scores);
    checksum = mix_scores(checksum, scores, predicted_class);
    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t run_opt_one(const int8_t *input, uint8_t *scores)
{
    uint32_t checksum = 0x54554652u;

    tiny_infer_opt_one(input, scores);
    const uint8_t predicted_class = argmax_u8(scores);
    checksum = mix_scores(checksum, scores, predicted_class);
    sink = checksum;
    return checksum;
}

static infer_result_t measure_tflm_one(const int8_t *input, uint8_t *scores)
{
    infer_result_t result;

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_tflm_one(input, scores, &result.status);

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.predicted_class = argmax_u8(scores);
    result.pass = (result.status == 0u);

    return result;
}

static infer_result_t measure_opt_one(const int8_t *input, uint8_t *scores)
{
    infer_result_t result;

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_opt_one(input, scores);

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.status = 0u;
    result.predicted_class = argmax_u8(scores);
    result.pass = 1u;

    return result;
}

static uint32_t score_mismatch_count(void)
{
    uint32_t mismatches = 0u;

    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        if (tflm_scores[out] != opt_scores[out]) {
            ++mismatches;
        }
    }

    return mismatches;
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

    uint32_t status = 0u;
    (void)run_tflm_one(tiny_ai_input_samples[0], tflm_scores, &status);
    (void)run_opt_one(tiny_ai_input_samples[0], opt_scores);
    return status;
}

static uint32_t speedup_x100(uint32_t ref_cycles, uint32_t opt_cycles)
{
    if (opt_cycles == 0u) {
        return 0u;
    }

    return (ref_cycles * 100u + (opt_cycles / 2u)) / opt_cycles;
}

static void uart_print_scores_csv(const uint8_t *scores)
{
    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        if (out != 0u) {
            uart_putc(',');
        }
        uart_put_u32_dec(scores[out]);
    }
}

static void print_banner(void)
{
    uart_puts("\nDE2i-150 CV32E40P TFLM tiny UART inference\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n");
    uart_puts("Model: ");
    uart_puts(TINY_AI_MODEL_NAME);
    uart_puts(" int8 64 -> 16 -> 4\n");
    uart_puts("Frame: 55 aa cmd len_lo len_hi payload checksum_le32\n");
    uart_puts("Commands: 0x10 ping, 0x11 infer-one-64B\n");
    uart_puts("Ready\n");
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

static void print_ping(uint32_t seq, uint32_t status)
{
    uart_puts("OK seq=");
    uart_put_u32_dec(seq);
    uart_puts(" cmd=0x10 len=0 status=");
    uart_put_u32_hex8(status);
    uart_putc('\n');
}

static void print_infer_ok(uint32_t seq, const infer_result_t *ref,
    const infer_result_t *opt, uint32_t mismatches, uint32_t rx_status,
    uint32_t pass)
{
    uart_puts("OK seq=");
    uart_put_u32_dec(seq);
    uart_puts(" cmd=0x11 len=");
    uart_put_u32_dec(INPUT_DIM);
    uart_puts(" pass=");
    uart_puts(pass ? "yes" : "no");
    uart_puts(" ref_cls=");
    uart_put_u32_dec(ref->predicted_class);
    uart_puts(" opt_cls=");
    uart_put_u32_dec(opt->predicted_class);
    uart_puts(" mismatches=");
    uart_put_u32_dec(mismatches);
    uart_puts(" ref_cycles=");
    uart_put_u32_dec(ref->cycles);
    uart_puts(" opt_cycles=");
    uart_put_u32_dec(opt->cycles);
    uart_puts(" speedup=");
    uart_put_x100(speedup_x100(ref->cycles, opt->cycles));
    uart_putc('x');
    uart_puts(" ref_scores=");
    uart_print_scores_csv(tflm_scores);
    uart_puts(" opt_scores=");
    uart_print_scores_csv(opt_scores);
    uart_puts(" checksum=");
    uart_put_u32_hex8(ref->checksum);
    uart_puts(" opt_checksum=");
    uart_put_u32_hex8(opt->checksum);
    uart_puts(" ref_status=");
    uart_put_u32_hex8(ref->status);
    uart_puts(" opt_status=");
    uart_put_u32_hex8(opt->status);
    uart_puts(" rx_status=");
    uart_put_u32_hex8(rx_status);
    uart_puts(" cyc_mac_ref=");
    uart_put_x100(ratio_x100(ref->cycles, INT8_MACS_PER_SAMPLE));
    uart_puts(" cyc_mac_opt=");
    uart_put_x100(ratio_x100(opt->cycles, INT8_MACS_PER_SAMPLE));
    uart_putc('\n');
}

int main(void)
{
    uint32_t seq = 0u;

    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();
    uart_clear_rx_errors();

    LED_REG = LED_STATUS_SETUP;
    tflite::InitializeTarget();

    const tflite::Model *model =
        tflite::GetModel(g_tflm_tiny_ai_model_data);
    uint32_t setup_status = 0u;

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        setup_status = 0xe001u;
    } else {
        tflite::MicroMutableOpResolver<1> resolver;

        if (resolver.AddFullyConnected() != kTfLiteOk) {
            setup_status = 0xe002u;
        } else {
            tflite::MicroInterpreter interpreter(model, resolver,
                tensor_arena, sizeof(tensor_arena));

            setup_status = setup_tflm(&interpreter);
            if (setup_status == 0u) {
                LED_REG = LED_STATUS_WAIT;
                print_banner();
            }

            while (setup_status == 0u) {
                LED_REG = LED_STATUS_WAIT;
                wait_for_sync();
                uart_clear_rx_errors();

                LED_REG = LED_STATUS_RECV;
                const uint8_t cmd = uart_getc_blocking();
                const uint16_t len_lo = uart_getc_blocking();
                const uint16_t len_hi = uart_getc_blocking();
                const uint16_t len = (uint16_t)(len_lo | (len_hi << 8));
                uint32_t checksum = frame_checksum_header(cmd, len);
                uint32_t received_checksum = 0u;
                uint32_t rx_status = 0u;

                ++seq;

                if (len > RX_MAX_PAYLOAD) {
                    for (uint32_t i = 0u; i < len; ++i) {
                        const uint8_t value = uart_getc_blocking();
                        checksum = frame_checksum_update(checksum, value);
                        sink = value;
                    }
                    received_checksum = read_u32_le();
                    rx_status = uart_get_status();
                    LED_REG = LED_STATUS_FAIL;
                    print_err(seq, 0xe010u, checksum,
                        received_checksum, rx_status);
                    continue;
                }

                for (uint32_t i = 0u; i < len; ++i) {
                    payload[i] = uart_getc_blocking();
                    checksum = frame_checksum_update(checksum, payload[i]);
                }
                received_checksum = read_u32_le();
                rx_status = uart_get_status();

                if (checksum != received_checksum) {
                    LED_REG = LED_STATUS_FAIL;
                    print_err(seq, 0xe011u, checksum,
                        received_checksum, rx_status);
                    continue;
                }

                if ((rx_status & UART_STATUS_RX_ERROR_MASK) != 0u) {
                    LED_REG = LED_STATUS_FAIL;
                    print_err(seq, 0xe012u, checksum,
                        received_checksum, rx_status);
                    continue;
                }

                if (cmd == RX_CMD_PING) {
                    if (len != 0u) {
                        LED_REG = LED_STATUS_FAIL;
                        print_err(seq, 0xe013u, checksum,
                            received_checksum, rx_status);
                    } else {
                        LED_REG = LED_STATUS_PASS;
                        print_ping(seq, rx_status);
                    }
                    continue;
                }

                if (cmd != RX_CMD_INFER || len != INPUT_DIM) {
                    LED_REG = LED_STATUS_FAIL;
                    print_err(seq, 0xe014u, checksum,
                        received_checksum, rx_status);
                    continue;
                }

                for (uint32_t i = 0u; i < INPUT_DIM; ++i) {
                    runtime_input[i] = (int8_t)payload[i];
                }

                LED_REG = LED_STATUS_REF;
                const infer_result_t ref =
                    measure_tflm_one(runtime_input, tflm_scores);

                LED_REG = LED_STATUS_OPT;
                const infer_result_t opt =
                    measure_opt_one(runtime_input, opt_scores);

                const uint32_t mismatches = score_mismatch_count();
                const uint32_t pass = ref.pass && opt.pass &&
                    (ref.checksum == opt.checksum) &&
                    (ref.predicted_class == opt.predicted_class) &&
                    (mismatches == 0u);

                LED_REG = pass ? LED_STATUS_PASS : LED_STATUS_FAIL;
                print_infer_ok(seq, &ref, &opt, mismatches, rx_status, pass);
            }
        }
    }

    if (setup_status != 0u) {
        LED_REG = LED_STATUS_FAIL;
        uart_puts("FATAL setup_status=");
        uart_put_u32_hex8(setup_status);
        uart_putc('\n');
    }

    while (1) {
        LED_REG = LED_STATUS_FAIL;
        delay_cycles(50000000u);
    }
}
