#include <stdint.h>

#include "ai_ops.h"
#include "mnist_fc/mnist_fc_model_data.h"
#include "perf.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#define CLOCK_HZ 50000000u
#define TFLM_MNIST_TENSOR_ARENA_SIZE 12288u

#define INPUT_DIM    784u
#define HIDDEN_DIM   32u
#define OUTPUT_DIM   10u

#define INPUT_WORDS  (INPUT_DIM / 4u)
#define HIDDEN_WORDS (HIDDEN_DIM / 4u)
#define MAX_FC_OUTPUT_DIM HIDDEN_DIM

#define INT8_MACS_PER_SAMPLE \
    ((HIDDEN_DIM * INPUT_DIM) + (OUTPUT_DIM * HIDDEN_DIM))

#define RX_SYNC0 0x55u
#define RX_SYNC1 0xaau
#define RX_CMD_PING  0x10u
#define RX_CMD_INFER 0x11u
#define RX_MAX_PAYLOAD INPUT_DIM

#define LED_STATUS_INIT      0x98u
#define LED_STATUS_SETUP     0x99u
#define LED_STATUS_WAIT      0x9au
#define LED_STATUS_RECV      0x9bu
#define LED_STATUS_REF       0x9cu
#define LED_STATUS_OPT       0x9du
#define LED_STATUS_PASS      0xa5u
#define LED_STATUS_FAIL      0xefu

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
    uint32_t status;
    uint8_t predicted_class;
    uint8_t pass;
} infer_result_t;

typedef struct {
    const int8_t *weights;
    const int32_t *bias;
    int32_t bias_values[MAX_FC_OUTPUT_DIM];
    uint32_t input_dim;
    uint32_t output_dim;
    int32_t input_offset;
    int32_t output_offset;
    int32_t activation_min;
    int32_t activation_max;
    int32_t weight_sums[MAX_FC_OUTPUT_DIM];
    int32_t output_multiplier[MAX_FC_OUTPUT_DIM];
    int output_shift[MAX_FC_OUTPUT_DIM];
} fc_opt_params_t;

static uint8_t tensor_arena[TFLM_MNIST_TENSOR_ARENA_SIZE]
    __attribute__((aligned(16)));
static uint8_t payload[RX_MAX_PAYLOAD] __attribute__((aligned(4)));
static int8_t runtime_input[INPUT_DIM] __attribute__((aligned(4)));
static int8_t tflm_scores[OUTPUT_DIM];
static int8_t opt_scores[OUTPUT_DIM];
static fc_opt_params_t fc1_params;
static fc_opt_params_t fc2_params;

static tflite::MicroInterpreter *g_interpreter;
static TfLiteTensor *g_input;
static TfLiteTensor *g_output;
static volatile uint32_t sink;
static uint32_t arena_used_bytes;

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

static void uart_put_i32_dec(int32_t value)
{
    if (value < 0) {
        uart_putc('-');
        uart_put_u32_dec((uint32_t)(-value));
    } else {
        uart_put_u32_dec((uint32_t)value);
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

static uint32_t speedup_x100(uint32_t ref_cycles, uint32_t opt_cycles)
{
    if (opt_cycles == 0u) {
        return 0u;
    }

    return (ref_cycles * 100u + (opt_cycles / 2u)) / opt_cycles;
}

static uint8_t argmax_i8(const int8_t *scores)
{
    uint8_t best = 0u;
    int8_t best_score = scores[0];

    for (uint32_t i = 1u; i < OUTPUT_DIM; ++i) {
        if (scores[i] > best_score) {
            best = (uint8_t)i;
            best_score = scores[i];
        }
    }

    return best;
}

static int32_t clamp_i32(int32_t value, int32_t min_value,
    int32_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int32_t round_to_i32(float value)
{
    if (value >= 0.0f) {
        return (int32_t)(value + 0.5f);
    }
    return (int32_t)(value - 0.5f);
}

static int32_t quantize_activation_i8(float value, float scale,
    int32_t zero_point)
{
    const int32_t quantized =
        zero_point + round_to_i32(value / scale);
    return clamp_i32(quantized, -128, 127);
}

static uint32_t activation_range_i8(tflite::ActivationFunctionType activation,
    float output_scale, int32_t output_zero_point, int32_t *act_min,
    int32_t *act_max)
{
    if (act_min == nullptr || act_max == nullptr || output_scale == 0.0f) {
        return 0xe020u;
    }

    switch (activation) {
      case tflite::ActivationFunctionType_RELU:
        *act_min = quantize_activation_i8(0.0f, output_scale,
            output_zero_point);
        *act_max = 127;
        return 0u;
      case tflite::ActivationFunctionType_RELU6:
        *act_min = quantize_activation_i8(0.0f, output_scale,
            output_zero_point);
        *act_max = quantize_activation_i8(6.0f, output_scale,
            output_zero_point);
        return 0u;
      case tflite::ActivationFunctionType_RELU_N1_TO_1:
        *act_min = quantize_activation_i8(-1.0f, output_scale,
            output_zero_point);
        *act_max = quantize_activation_i8(1.0f, output_scale,
            output_zero_point);
        return 0u;
      case tflite::ActivationFunctionType_NONE:
        *act_min = -128;
        *act_max = 127;
        return 0u;
      default:
        return 0xe021u;
    }
}

static uint32_t tensor_flat_size(const tflite::Tensor *tensor)
{
    const auto *shape = tensor == nullptr ? nullptr : tensor->shape();
    if (shape == nullptr || shape->size() == 0u) {
        return 0u;
    }

    uint32_t size = 1u;
    for (uint32_t i = 0u; i < shape->size(); ++i) {
        const int32_t dim = shape->Get(i);
        if (dim <= 0) {
            return 0u;
        }
        size *= (uint32_t)dim;
    }
    return size;
}

static uint32_t tensor_last_dim(const tflite::Tensor *tensor)
{
    const auto *shape = tensor == nullptr ? nullptr : tensor->shape();
    if (shape == nullptr || shape->size() == 0u) {
        return 0u;
    }
    const int32_t dim = shape->Get(shape->size() - 1u);
    if (dim <= 0) {
        return 0u;
    }
    return (uint32_t)dim;
}

static uint32_t read_tensor_quant(const tflite::Tensor *tensor, float *scale,
    int32_t *zero_point)
{
    const auto *quant = tensor == nullptr ? nullptr : tensor->quantization();
    const auto *scales = quant == nullptr ? nullptr : quant->scale();
    const auto *zero_points = quant == nullptr ? nullptr :
        quant->zero_point();

    if (scale == nullptr || zero_point == nullptr || scales == nullptr ||
        zero_points == nullptr || scales->size() == 0u ||
        zero_points->size() == 0u) {
        return 0xe022u;
    }

    *scale = scales->Get(0);
    *zero_point = (int32_t)zero_points->Get(0);
    return 0u;
}

static uint32_t read_tensor_buffer(const tflite::Model *model,
    const tflite::Tensor *tensor, const uint8_t **data, uint32_t *size)
{
    const auto *buffers = model == nullptr ? nullptr : model->buffers();
    if (tensor == nullptr || data == nullptr || size == nullptr ||
        buffers == nullptr || tensor->buffer() >= buffers->size()) {
        return 0xe036u;
    }

    const auto *buffer = buffers->Get(tensor->buffer());
    const auto *bytes = buffer == nullptr ? nullptr : buffer->data();
    if (bytes == nullptr || bytes->data() == nullptr) {
        return 0xe036u;
    }

    *data = bytes->data();
    *size = bytes->size();
    return 0u;
}

static int32_t read_i32_le_unaligned(const uint8_t *data)
{
    return (int32_t)((uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24));
}

static uint32_t pack4_i8(const int8_t *data)
{
    return ((uint32_t)(uint8_t)data[0]) |
        ((uint32_t)(uint8_t)data[1] << 8) |
        ((uint32_t)(uint8_t)data[2] << 16) |
        ((uint32_t)(uint8_t)data[3] << 24);
}

static uint32_t is_word_aligned(const void *ptr)
{
    return (((uintptr_t)ptr & 3u) == 0u);
}

__attribute__((noinline))
static int32_t dot4_i8(const int8_t *activation, const int8_t *weight,
    uint32_t word_count)
{
    int32_t acc = 0;

    if (is_word_aligned(activation) && is_word_aligned(weight)) {
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
            : [acc] "+r" (acc),
              [act_ptr] "+r" (act_ptr),
              [weight_ptr] "+r" (weight_ptr),
              [act_word] "=&r" (act_word),
              [weight_word] "=&r" (weight_word)
            : [count] "r" (word_count)
            : "memory"
        );

        return acc;
    }

    for (uint32_t word = 0u; word < word_count; ++word) {
        const uint32_t act_word = pack4_i8(&activation[word * 4u]);
        const uint32_t weight_word = pack4_i8(&weight[word * 4u]);
        acc = ai_dot4_acc(acc, act_word, weight_word);
    }

    return acc;
}

__attribute__((noinline))
static void dot4_i8x4_stream_loads(const int8_t *activation,
    const int8_t *weight0, uint32_t row_stride, uint32_t word_count,
    int32_t *accs)
{
    uintptr_t act_ptr = (uintptr_t)activation;
    uintptr_t weight0_ptr = (uintptr_t)weight0;
    uintptr_t weight1_ptr = (uintptr_t)(weight0 + row_stride);
    uintptr_t weight2_ptr = (uintptr_t)(weight0 + (2u * row_stride));
    uintptr_t weight3_ptr = (uintptr_t)(weight0 + (3u * row_stride));
    uint32_t act_word;
    uint32_t weight_word;
    int32_t acc0 = accs[0];
    int32_t acc1 = accs[1];
    int32_t acc2 = accs[2];
    int32_t acc3 = accs[3];

    __asm__ volatile (
        ".option push\n"
        ".option norvc\n"
        ".balign 4\n"
        ".insn i 0x2b, 4, x14, %[count], 10\n"
        ".insn i 0x0b, 2, %[act_word], %[act_ptr], 4\n"
        ".insn i 0x0b, 2, %[weight_word], %[weight0_ptr], 4\n"
        ".insn r 0x7b, 1, 0x54, %[acc0], %[act_word], %[weight_word]\n"
        ".insn i 0x0b, 2, %[weight_word], %[weight1_ptr], 4\n"
        ".insn r 0x7b, 1, 0x54, %[acc1], %[act_word], %[weight_word]\n"
        ".insn i 0x0b, 2, %[weight_word], %[weight2_ptr], 4\n"
        ".insn r 0x7b, 1, 0x54, %[acc2], %[act_word], %[weight_word]\n"
        ".insn i 0x0b, 2, %[weight_word], %[weight3_ptr], 4\n"
        ".insn r 0x7b, 1, 0x54, %[acc3], %[act_word], %[weight_word]\n"
        ".option pop\n"
        : [acc0] "+r" (acc0),
          [acc1] "+r" (acc1),
          [acc2] "+r" (acc2),
          [acc3] "+r" (acc3),
          [act_ptr] "+r" (act_ptr),
          [weight0_ptr] "+r" (weight0_ptr),
          [weight1_ptr] "+r" (weight1_ptr),
          [weight2_ptr] "+r" (weight2_ptr),
          [weight3_ptr] "+r" (weight3_ptr),
          [act_word] "=&r" (act_word),
          [weight_word] "=&r" (weight_word)
        : [count] "r" (word_count)
        : "memory"
    );

    accs[0] = acc0;
    accs[1] = acc1;
    accs[2] = acc2;
    accs[3] = acc3;
}

static int8_t requant_fc_output(const fc_opt_params_t *params,
    uint32_t out, int32_t acc)
{
    acc += params->input_offset * params->weight_sums[out];
    int32_t value = tflite::MultiplyByQuantizedMultiplier(acc,
        params->output_multiplier[out], params->output_shift[out]);
    value += params->output_offset;
    value = clamp_i32(value, params->activation_min,
        params->activation_max);
    return (int8_t)value;
}

static uint32_t setup_fc_opt_layer(const tflite::Model *model,
    const tflite::SubGraph *subgraph, uint32_t op_index,
    fc_opt_params_t *params, uint32_t expected_input_dim,
    uint32_t expected_output_dim)
{
    const auto *operators = subgraph == nullptr ? nullptr :
        subgraph->operators();
    const auto *tensors = subgraph == nullptr ? nullptr :
        subgraph->tensors();
    const auto *opcodes = model == nullptr ? nullptr :
        model->operator_codes();

    if (operators == nullptr || tensors == nullptr || opcodes == nullptr ||
        op_index >= operators->size() || params == nullptr) {
        return 0xe030u;
    }

    const auto *op = operators->Get(op_index);
    const auto *inputs = op == nullptr ? nullptr : op->inputs();
    const auto *outputs = op == nullptr ? nullptr : op->outputs();
    if (op == nullptr || inputs == nullptr || outputs == nullptr ||
        inputs->size() < 3u || outputs->size() < 1u ||
        op->opcode_index() >= opcodes->size()) {
        return 0xe031u;
    }

    const auto *opcode = opcodes->Get(op->opcode_index());
    if (opcode == nullptr) {
        return 0xe032u;
    }
    const tflite::BuiltinOperator deprecated_builtin =
        (tflite::BuiltinOperator)opcode->deprecated_builtin_code();
    if (opcode->builtin_code() !=
        tflite::BuiltinOperator_FULLY_CONNECTED &&
        deprecated_builtin != tflite::BuiltinOperator_FULLY_CONNECTED) {
        return 0xe032u;
    }

    const int32_t input_index = inputs->Get(0);
    const int32_t weight_index = inputs->Get(1);
    const int32_t bias_index = inputs->Get(2);
    const int32_t output_index = outputs->Get(0);
    if (input_index < 0 || weight_index < 0 || bias_index < 0 ||
        output_index < 0 ||
        (uint32_t)input_index >= tensors->size() ||
        (uint32_t)weight_index >= tensors->size() ||
        (uint32_t)bias_index >= tensors->size() ||
        (uint32_t)output_index >= tensors->size()) {
        return 0xe033u;
    }

    const auto *input_tensor = tensors->Get((uint32_t)input_index);
    const auto *weight_tensor = tensors->Get((uint32_t)weight_index);
    const auto *bias_tensor = tensors->Get((uint32_t)bias_index);
    const auto *output_tensor = tensors->Get((uint32_t)output_index);
    if (input_tensor == nullptr || weight_tensor == nullptr ||
        bias_tensor == nullptr || output_tensor == nullptr ||
        input_tensor->type() != tflite::TensorType_INT8 ||
        weight_tensor->type() != tflite::TensorType_INT8 ||
        bias_tensor->type() != tflite::TensorType_INT32 ||
        output_tensor->type() != tflite::TensorType_INT8) {
        return 0xe034u;
    }

    if (tensor_flat_size(input_tensor) != expected_input_dim ||
        tensor_flat_size(weight_tensor) !=
            expected_input_dim * expected_output_dim ||
        tensor_flat_size(bias_tensor) != expected_output_dim ||
        tensor_last_dim(output_tensor) != expected_output_dim ||
        expected_output_dim > MAX_FC_OUTPUT_DIM ||
        (expected_input_dim % 4u) != 0u) {
        return 0xe035u;
    }

    const uint8_t *weight_data;
    uint32_t weight_bytes;
    uint32_t status = read_tensor_buffer(model, weight_tensor, &weight_data,
        &weight_bytes);
    if (status != 0u) {
        return status;
    }

    const uint8_t *bias_data;
    uint32_t bias_bytes;
    status = read_tensor_buffer(model, bias_tensor, &bias_data, &bias_bytes);
    if (status != 0u) {
        return status;
    }

    if (weight_bytes != expected_input_dim * expected_output_dim ||
        bias_bytes != expected_output_dim * sizeof(int32_t)) {
        return 0xe036u;
    }

    float input_scale;
    int32_t input_zero_point;
    float output_scale;
    int32_t output_zero_point;
    status = read_tensor_quant(input_tensor, &input_scale,
        &input_zero_point);
    if (status != 0u) {
        return status;
    }
    status = read_tensor_quant(output_tensor, &output_scale,
        &output_zero_point);
    if (status != 0u) {
        return status;
    }

    const auto *weight_quant = weight_tensor->quantization();
    const auto *weight_scales = weight_quant == nullptr ? nullptr :
        weight_quant->scale();
    const auto *weight_zero_points = weight_quant == nullptr ? nullptr :
        weight_quant->zero_point();
    if (weight_scales == nullptr || weight_zero_points == nullptr ||
        weight_scales->size() == 0u || weight_zero_points->size() == 0u ||
        !(weight_scales->size() == 1u ||
          weight_scales->size() == expected_output_dim) ||
        !(weight_zero_points->size() == 1u ||
          weight_zero_points->size() == expected_output_dim)) {
        return 0xe037u;
    }

    tflite::ActivationFunctionType activation =
        tflite::ActivationFunctionType_NONE;
    const auto *options = op->builtin_options_as_FullyConnectedOptions();
    if (options != nullptr) {
        activation = options->fused_activation_function();
    }

    params->weights = (const int8_t *)weight_data;
    params->bias = params->bias_values;
    params->input_dim = expected_input_dim;
    params->output_dim = expected_output_dim;
    params->input_offset = -input_zero_point;
    params->output_offset = output_zero_point;

    status = activation_range_i8(activation, output_scale,
        output_zero_point, &params->activation_min,
        &params->activation_max);
    if (status != 0u) {
        return status;
    }

    for (uint32_t out = 0u; out < expected_output_dim; ++out) {
        params->bias_values[out] =
            read_i32_le_unaligned(&bias_data[out * sizeof(int32_t)]);
    }

    for (uint32_t out = 0u; out < expected_output_dim; ++out) {
        const uint32_t scale_index =
            weight_scales->size() == 1u ? 0u : out;
        const uint32_t zero_point_index =
            weight_zero_points->size() == 1u ? 0u : out;
        const int32_t weight_zero_point =
            (int32_t)weight_zero_points->Get(zero_point_index);
        if (weight_zero_point != 0) {
            return 0xe038u;
        }

        const double effective_scale =
            (double)input_scale * (double)weight_scales->Get(scale_index) /
            (double)output_scale;
        tflite::QuantizeMultiplier(effective_scale,
            &params->output_multiplier[out], &params->output_shift[out]);

        int32_t weight_sum = 0;
        const int8_t *row = &params->weights[out * expected_input_dim];
        for (uint32_t i = 0u; i < expected_input_dim; ++i) {
            weight_sum += row[i];
        }
        params->weight_sums[out] = weight_sum;
    }

    return 0u;
}

static uint32_t setup_pulp_opt_params(const tflite::Model *model)
{
    const auto *subgraphs = model == nullptr ? nullptr : model->subgraphs();
    if (subgraphs == nullptr || subgraphs->size() == 0u) {
        return 0xe040u;
    }

    const auto *subgraph = subgraphs->Get(0);
    const auto *operators = subgraph == nullptr ? nullptr :
        subgraph->operators();
    if (operators == nullptr || operators->size() != 2u) {
        return 0xe041u;
    }

    uint32_t status = setup_fc_opt_layer(model, subgraph, 0u, &fc1_params,
        INPUT_DIM, HIDDEN_DIM);
    if (status != 0u) {
        return status;
    }

    status = setup_fc_opt_layer(model, subgraph, 1u, &fc2_params,
        HIDDEN_DIM, OUTPUT_DIM);
    return status;
}

static void mnist_fc_infer_opt_one(const int8_t *input, int8_t *output)
{
    int8_t hidden[HIDDEN_DIM] __attribute__((aligned(4)));

    uint32_t h = 0u;
    for (; h + 3u < HIDDEN_DIM; h += 4u) {
        const int8_t *weight =
            &fc1_params.weights[h * fc1_params.input_dim];
        int32_t accs[4] = {
            fc1_params.bias[h],
            fc1_params.bias[h + 1u],
            fc1_params.bias[h + 2u],
            fc1_params.bias[h + 3u],
        };

        if (is_word_aligned(input) && is_word_aligned(weight)) {
            dot4_i8x4_stream_loads(input, weight, fc1_params.input_dim,
                INPUT_WORDS, accs);
        } else {
            accs[0] += dot4_i8(input, weight, INPUT_WORDS);
            accs[1] += dot4_i8(input, weight + fc1_params.input_dim,
                INPUT_WORDS);
            accs[2] += dot4_i8(input, weight + (2u * fc1_params.input_dim),
                INPUT_WORDS);
            accs[3] += dot4_i8(input, weight + (3u * fc1_params.input_dim),
                INPUT_WORDS);
        }

        hidden[h] = requant_fc_output(&fc1_params, h, accs[0]);
        hidden[h + 1u] = requant_fc_output(&fc1_params, h + 1u, accs[1]);
        hidden[h + 2u] = requant_fc_output(&fc1_params, h + 2u, accs[2]);
        hidden[h + 3u] = requant_fc_output(&fc1_params, h + 3u, accs[3]);
    }

    for (; h < HIDDEN_DIM; ++h) {
        const int8_t *weight =
            &fc1_params.weights[h * fc1_params.input_dim];
        int32_t acc = fc1_params.bias[h] +
            dot4_i8(input, weight, INPUT_WORDS);
        hidden[h] = requant_fc_output(&fc1_params, h, acc);
    }

    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        const int8_t *weight =
            &fc2_params.weights[out * fc2_params.input_dim];
        int32_t acc = fc2_params.bias[out] +
            dot4_i8(hidden, weight, HIDDEN_WORDS);
        output[out] = requant_fc_output(&fc2_params, out, acc);
    }
}

static void load_tflm_input(const int8_t *input)
{
    for (uint32_t i = 0u; i < INPUT_DIM; ++i) {
        g_input->data.int8[i] = input[i];
    }
}

static uint32_t mix_runtime_scores(uint32_t checksum, const int8_t *outputs,
    uint8_t predicted_class)
{
    checksum = mix32(checksum, predicted_class);
    for (uint32_t i = 0u; i < OUTPUT_DIM; ++i) {
        const uint32_t byte = (uint8_t)outputs[i];
        checksum = mix32(checksum, byte ^ (i * 0x045d9f3bu));
    }
    return checksum;
}

__attribute__((noinline))
static uint32_t run_tflm_one(const int8_t *input, int8_t *scores,
    uint32_t *status)
{
    uint32_t checksum = 0x4d554152u;

    *status = 0u;
    load_tflm_input(input);

    if (g_interpreter->Invoke() != kTfLiteOk) {
        *status = 0xe006u;
        sink = *status;
        return mix32(checksum, *status);
    }

    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        scores[out] = g_output->data.int8[out];
    }

    checksum = mix_runtime_scores(checksum, scores, argmax_i8(scores));
    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t run_opt_one(const int8_t *input, int8_t *scores)
{
    uint32_t checksum = 0x4d554152u;

    mnist_fc_infer_opt_one(input, scores);
    checksum = mix_runtime_scores(checksum, scores, argmax_i8(scores));
    sink = checksum;
    return checksum;
}

static infer_result_t measure_tflm_one(const int8_t *input, int8_t *scores)
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
    result.pass = (result.status == 0u);
    result.predicted_class = result.pass ? argmax_i8(scores) : 255u;

    return result;
}

static infer_result_t measure_opt_one(const int8_t *input, int8_t *scores)
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
    result.predicted_class = argmax_i8(scores);
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

static uint32_t warmup_tflm(void)
{
    for (uint32_t i = 0u; i < INPUT_DIM; ++i) {
        runtime_input[i] = -128;
    }

    uint32_t status = 0u;
    (void)run_tflm_one(runtime_input, tflm_scores, &status);
    (void)run_opt_one(runtime_input, opt_scores);
    return status;
}

static uint32_t setup_tflm(tflite::MicroInterpreter *interpreter,
    const tflite::Model *model)
{
    g_interpreter = interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        return 0xe003u;
    }

    arena_used_bytes = (uint32_t)interpreter->arena_used_bytes();
    g_input = interpreter->input(0);
    g_output = interpreter->output(0);
    if (g_input == nullptr || g_output == nullptr) {
        return 0xe004u;
    }

    if (g_input->type != kTfLiteInt8 || g_output->type != kTfLiteInt8 ||
        g_input->bytes != INPUT_DIM || g_output->bytes != OUTPUT_DIM) {
        return 0xe005u;
    }

    uint32_t status = setup_pulp_opt_params(model);
    if (status != 0u) {
        return status;
    }

    status = warmup_tflm();
    if (status != 0u) {
        return status;
    }

    return 0u;
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

static void uart_print_scores_csv(const int8_t *scores)
{
    for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
        if (out != 0u) {
            uart_putc(',');
        }
        uart_put_i32_dec(scores[out]);
    }
}

static void print_banner(void)
{
    uart_puts("\nDE2i-150 CV32E40P TFLM MNIST FC UART inference\n");
    uart_puts("UART: 115200 8N1\n");
    uart_puts("Clock: ");
    uart_put_u32_dec(CLOCK_HZ);
    uart_puts(" Hz\n");
    uart_puts("Model: MNIST FC int8 784 -> 32 -> 10\n");
    uart_puts("Model bytes: ");
    uart_put_u32_dec(g_mnist_fc_model_data_size);
    uart_puts("\nTensor arena: ");
    uart_put_u32_dec(TFLM_MNIST_TENSOR_ARENA_SIZE);
    uart_puts(" bytes, used ");
    uart_put_u32_dec(arena_used_bytes);
    uart_puts(" bytes\nFrame: 55 aa cmd len_lo len_hi payload checksum_le32\n");
    uart_puts("Commands: 0x10 ping, 0x11 infer-one-784B-int8\n");
    uart_puts("Optimized kernel: cv.sdotsp.b + aligned cv.lw/cv.setup FC1x4 fast path\n");
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

    const tflite::Model *model = tflite::GetModel(g_mnist_fc_model_data);
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

            setup_status = setup_tflm(&interpreter, model);
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
