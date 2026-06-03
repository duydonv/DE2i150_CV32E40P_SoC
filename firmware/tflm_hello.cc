#include <stdint.h>

#include "models/hello_world_int8_model_data.h"
#include "perf.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#define CLOCK_HZ 50000000u
#define TFLM_TENSOR_ARENA_SIZE 4096u
#define TFLM_INPUT_COUNT 4u

#define LED_STATUS_INIT       0x51u
#define LED_STATUS_SETUP      0x52u
#define LED_STATUS_INVOKE     0x53u
#define LED_STATUS_ALL_PASS   0xa5u
#define LED_STATUS_FAIL       0xefu

#define REPORT_DELAY_CYCLES 50000000u

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
    uint32_t pass;
    uint32_t status;
} tflm_result_t;

static const int8_t tflm_inputs[TFLM_INPUT_COUNT] = {-96, -63, -34, 0};
static int8_t tflm_outputs[TFLM_INPUT_COUNT];
static uint8_t tensor_arena[TFLM_TENSOR_ARENA_SIZE]
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

static void uart_put_i32_dec(int32_t value)
{
    uint32_t magnitude;

    if (value < 0) {
        uart_putc('-');
        magnitude = (uint32_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint32_t)value;
    }

    uart_put_u32_dec(magnitude);
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

static uint32_t ratio_x100(uint32_t numerator, uint32_t denominator)
{
    if (denominator == 0u) {
        return 0u;
    }

    return (numerator * 100u + (denominator / 2u)) / denominator;
}

static void uart_print_i8_list(const int8_t *values)
{
    for (uint32_t i = 0; i < TFLM_INPUT_COUNT; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_i32_dec(values[i]);
    }
}

__attribute__((noinline))
static uint32_t run_tflm_invokes(void)
{
    uint32_t checksum = 0x54464c4du;
    run_status = 0u;

    for (uint32_t i = 0; i < TFLM_INPUT_COUNT; ++i) {
        g_input->data.int8[0] = tflm_inputs[i];

        if (g_interpreter->Invoke() != kTfLiteOk) {
            run_status = 0xe005u;
            sink = run_status;
            return mix32(checksum, run_status);
        }

        tflm_outputs[i] = g_output->data.int8[0];
        checksum = mix32(checksum, ((uint32_t)(uint8_t)tflm_outputs[i]) ^
            (i * 0x45d9f3bu));
    }

    sink = checksum;
    return checksum;
}

static tflm_result_t measure_tflm_invokes(void)
{
    tflm_result_t result;

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_tflm_invokes();

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.status = run_status;
    result.pass = (run_status == 0u);

    return result;
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

    if (run_tflm_invokes() == 0u || run_status != 0u) {
        return run_status != 0u ? run_status : 0xe006u;
    }

    return 0u;
}

static void uart_print_report(const tflm_result_t *result)
{
    uart_puts("\nDE2i-150 CV32E40P TFLM hello_world int8\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n");
    uart_puts("Model: hello_world int8 FullyConnected\n");
    uart_puts("Model bytes: ");
    uart_put_u32_dec(g_hello_world_int8_model_data_size);
    uart_puts("\nTensor arena: ");
    uart_put_u32_dec(TFLM_TENSOR_ARENA_SIZE);
    uart_puts(" bytes\n");
    uart_puts("Invokes: ");
    uart_put_u32_dec(TFLM_INPUT_COUNT);
    uart_puts("\n\n");

    uart_puts("Cycles    Instret  Cyc/invoke  Checksum    Status      Pass\n");
    uart_puts("--------  --------  ----------  ----------  ----------  ----\n");
    uart_put_u32_dec_right(result->cycles, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->instret, 8u);
    uart_puts("  ");
    uart_put_u32_dec_right(result->cycles / TFLM_INPUT_COUNT, 10u);
    uart_puts("  ");
    uart_put_u32_hex8(result->checksum);
    uart_puts("  ");
    uart_put_u32_hex8(result->status);
    uart_puts("  ");
    uart_puts(result->pass ? "yes" : "no");
    uart_puts("\n\nInputs int8:  ");
    uart_print_i8_list(tflm_inputs);
    uart_puts("\nOutputs int8: ");
    uart_print_i8_list(tflm_outputs);
    uart_puts("\nCycles/input x100: ");
    uart_put_x100(ratio_x100(result->cycles, TFLM_INPUT_COUNT));
    uart_puts("\n\n");
}

int main(void)
{
    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();

    LED_REG = LED_STATUS_SETUP;
    tflite::InitializeTarget();

    const tflite::Model *model =
        tflite::GetModel(g_hello_world_int8_model_data);
    tflm_result_t result = {};

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        result.status = 0xe001u;
    } else {
        tflite::MicroMutableOpResolver<1> resolver;

        if (resolver.AddFullyConnected() != kTfLiteOk) {
            result.status = 0xe002u;
        } else {
            tflite::MicroInterpreter interpreter(model, resolver,
                tensor_arena, sizeof(tensor_arena));

            result.status = setup_tflm(&interpreter);
            if (result.status == 0u) {
                LED_REG = LED_STATUS_INVOKE;
                result = measure_tflm_invokes();
            }
        }
    }

    result.pass = (result.status == 0u);
    LED_REG = result.pass ? LED_STATUS_ALL_PASS : LED_STATUS_FAIL;

    while (1) {
        uart_print_report(&result);
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}
