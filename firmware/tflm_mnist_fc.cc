#include <stdint.h>

#include "ai_ops.h"
#include "mnist_fc/mnist_fc_model_data.h"
#include "mnist_fc/mnist_fc_test_vectors.h"
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

#define INPUT_DIM    MNIST_FC_INPUT_DIM
#define HIDDEN_DIM   MNIST_FC_HIDDEN_DIM
#define OUTPUT_DIM   MNIST_FC_OUTPUT_DIM
#define SAMPLE_COUNT MNIST_FC_TEST_VECTOR_COUNT

#define INPUT_WORDS  (INPUT_DIM / 4u)
#define HIDDEN_WORDS (HIDDEN_DIM / 4u)
#define MAX_FC_OUTPUT_DIM HIDDEN_DIM

#define INT8_MACS_PER_SAMPLE \
    ((HIDDEN_DIM * INPUT_DIM) + (OUTPUT_DIM * HIDDEN_DIM))
#define TOTAL_INT8_MACS (SAMPLE_COUNT * INT8_MACS_PER_SAMPLE)
#define TOTAL_DOT4_OPS  (TOTAL_INT8_MACS / 4u)

#define LED_STATUS_INIT      0x90u
#define LED_STATUS_SETUP     0x91u
#define LED_STATUS_REF       0x92u
#define LED_STATUS_OPT       0x93u
#define LED_STATUS_ALL_PASS  0xa5u
#define LED_STATUS_FAIL      0xefu

#define REPORT_DELAY_CYCLES 50000000u

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t checksum;
    uint32_t label_matches;
    uint32_t class_matches;
    uint32_t score_mismatches;
    uint32_t status;
    uint32_t pass;
} run_result_t;

typedef struct {
    run_result_t ref;
    run_result_t opt;
    uint32_t speedup_x100;
    uint32_t class_mismatches;
    uint32_t score_mismatches;
    uint32_t pass;
} mnist_fc_result_t;

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
static int8_t tflm_outputs[SAMPLE_COUNT][OUTPUT_DIM];
static int8_t opt_outputs[SAMPLE_COUNT][OUTPUT_DIM];
static uint8_t tflm_classes[SAMPLE_COUNT];
static uint8_t opt_classes[SAMPLE_COUNT];
static fc_opt_params_t fc1_params;
static fc_opt_params_t fc2_params;

static tflite::MicroInterpreter *g_interpreter;
static TfLiteTensor *g_input;
static TfLiteTensor *g_output;
static volatile uint32_t sink;
static uint32_t run_status;
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

__attribute__((noinline))
static int32_t dot4_i8(const int8_t *activation, const int8_t *weight,
    uint32_t word_count)
{
    int32_t acc = 0;

    for (uint32_t word = 0u; word < word_count; ++word) {
        const uint32_t act_word = pack4_i8(&activation[word * 4u]);
        const uint32_t weight_word = pack4_i8(&weight[word * 4u]);
        acc = ai_dot4_acc(acc, act_word, weight_word);
    }

    return acc;
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

    for (uint32_t h = 0u; h < HIDDEN_DIM; ++h) {
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

static uint32_t mix_mnist_output(uint32_t checksum, const int8_t *outputs,
    uint8_t predicted_class, uint8_t label)
{
    checksum = mix32(checksum, predicted_class);
    checksum = mix32(checksum, label);
    for (uint32_t i = 0u; i < OUTPUT_DIM; ++i) {
        const uint32_t byte = (uint8_t)outputs[i];
        checksum = mix32(checksum, byte ^ (i * 0x045d9f3bu));
    }
    return checksum;
}

__attribute__((noinline))
static uint32_t run_tflm_mnist_samples(run_result_t *result)
{
    uint32_t checksum = 0x4d4e4953u;

    run_status = 0u;
    result->label_matches = 0u;
    result->class_matches = 0u;
    result->score_mismatches = 0u;

    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        for (uint32_t i = 0u; i < INPUT_DIM; ++i) {
            g_input->data.int8[i] = mnist_fc_test_inputs[sample][i];
        }

        if (g_interpreter->Invoke() != kTfLiteOk) {
            run_status = 0xe006u;
            sink = run_status;
            return mix32(checksum, run_status);
        }

        const int8_t *outputs = g_output->data.int8;
        const uint8_t predicted_class = argmax_i8(outputs);
        tflm_classes[sample] = predicted_class;

        if (predicted_class == mnist_fc_test_labels[sample]) {
            ++result->label_matches;
        }
        if (predicted_class == mnist_fc_test_expected_classes[sample]) {
            ++result->class_matches;
        }

        for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
            tflm_outputs[sample][out] = outputs[out];
            if ((uint8_t)outputs[out] !=
                mnist_fc_test_expected_output_bytes[sample][out]) {
                ++result->score_mismatches;
            }
        }

        checksum = mix_mnist_output(checksum, outputs, predicted_class,
            mnist_fc_test_labels[sample]);
    }

    sink = checksum;
    return checksum;
}

__attribute__((noinline))
static uint32_t run_opt_mnist_samples(run_result_t *result)
{
    uint32_t checksum = 0x4d4e4953u;

    result->label_matches = 0u;
    result->class_matches = 0u;
    result->score_mismatches = 0u;

    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        mnist_fc_infer_opt_one(mnist_fc_test_inputs[sample],
            opt_outputs[sample]);
        const uint8_t predicted_class = argmax_i8(opt_outputs[sample]);
        opt_classes[sample] = predicted_class;

        if (predicted_class == mnist_fc_test_labels[sample]) {
            ++result->label_matches;
        }
        if (predicted_class == mnist_fc_test_expected_classes[sample]) {
            ++result->class_matches;
        }

        for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
            if ((uint8_t)opt_outputs[sample][out] !=
                mnist_fc_test_expected_output_bytes[sample][out]) {
                ++result->score_mismatches;
            }
        }

        checksum = mix_mnist_output(checksum, opt_outputs[sample],
            predicted_class, mnist_fc_test_labels[sample]);
    }

    sink = checksum;
    return checksum;
}

static run_result_t measure_tflm_reference(void)
{
    run_result_t result = {};

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_tflm_mnist_samples(&result);

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.status = run_status;
    result.pass = (result.status == 0u) &&
        (result.checksum == MNIST_FC_TEST_CHECKSUM) &&
        (result.class_matches == SAMPLE_COUNT) &&
        (result.score_mismatches == 0u);

    return result;
}

static run_result_t measure_pulp_opt(void)
{
    run_result_t result = {};

    perf_fence();
    const uint32_t cycle_start = read_mcycle_lo();
    const uint32_t instret_start = read_minstret_lo();

    result.checksum = run_opt_mnist_samples(&result);

    perf_fence();
    const uint32_t instret_end = read_minstret_lo();
    const uint32_t cycle_end = read_mcycle_lo();

    result.cycles = cycle_end - cycle_start;
    result.instret = instret_end - instret_start;
    result.status = 0u;
    result.pass = (result.checksum == MNIST_FC_TEST_CHECKSUM) &&
        (result.class_matches == SAMPLE_COUNT) &&
        (result.score_mismatches == 0u);

    return result;
}

static uint32_t class_mismatch_count(void)
{
    uint32_t mismatches = 0u;

    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        if (tflm_classes[sample] != opt_classes[sample]) {
            ++mismatches;
        }
    }

    return mismatches;
}

static uint32_t score_mismatch_count(void)
{
    uint32_t mismatches = 0u;

    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        for (uint32_t out = 0u; out < OUTPUT_DIM; ++out) {
            if (tflm_outputs[sample][out] != opt_outputs[sample][out]) {
                ++mismatches;
            }
        }
    }

    return mismatches;
}

static void measure_ref_vs_opt(mnist_fc_result_t *result)
{
    (void)run_tflm_mnist_samples(&result->ref);
    (void)run_opt_mnist_samples(&result->opt);

    LED_REG = LED_STATUS_REF;
    result->ref = measure_tflm_reference();

    LED_REG = LED_STATUS_OPT;
    result->opt = measure_pulp_opt();

    result->speedup_x100 = speedup_x100(result->ref.cycles,
        result->opt.cycles);
    result->class_mismatches = class_mismatch_count();
    result->score_mismatches = score_mismatch_count();
    result->pass = result->ref.pass && result->opt.pass &&
        (result->ref.checksum == result->opt.checksum) &&
        (result->class_mismatches == 0u) &&
        (result->score_mismatches == 0u);
}

static uint32_t warmup_tflm(void)
{
    for (uint32_t i = 0u; i < INPUT_DIM; ++i) {
        g_input->data.int8[i] = mnist_fc_test_inputs[0][i];
    }

    if (g_interpreter->Invoke() != kTfLiteOk) {
        return 0xe006u;
    }

    return 0u;
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

static void uart_print_classes(const uint8_t *classes)
{
    for (uint32_t i = 0u; i < SAMPLE_COUNT; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec(classes[i]);
    }
}

static void uart_print_labels(void)
{
    for (uint32_t i = 0u; i < SAMPLE_COUNT; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec(mnist_fc_test_labels[i]);
    }
}

static void uart_print_expected_classes(void)
{
    for (uint32_t i = 0u; i < SAMPLE_COUNT; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_u32_dec(mnist_fc_test_expected_classes[i]);
    }
}

static void uart_print_sample0_outputs(const int8_t *outputs)
{
    for (uint32_t i = 0u; i < OUTPUT_DIM; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_i32_dec(outputs[i]);
    }
}

static void uart_print_expected_sample0_outputs(void)
{
    for (uint32_t i = 0u; i < OUTPUT_DIM; ++i) {
        if (i != 0u) {
            uart_putc(' ');
        }
        uart_put_i32_dec((int8_t)mnist_fc_test_expected_output_bytes[0][i]);
    }
}

static void uart_print_variant_row(const char *name,
    const run_result_t *result, uint32_t pass)
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

static void uart_print_report(const mnist_fc_result_t *result)
{
    uart_puts("\nDE2i-150 CV32E40P TFLM MNIST FC ref-vs-opt\n");
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
    uart_puts(" bytes\nSamples: ");
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nINT8 MACs: ");
    uart_put_u32_dec(TOTAL_INT8_MACS);
    uart_puts("\nCustom dot4 ops: ");
    uart_put_u32_dec(TOTAL_DOT4_OPS);
    uart_puts("\nOptimized kernel: cv.sdotsp.b dot4 + per-channel TFLite requant");
    uart_puts("\n\n");

    uart_puts("Variant   Cycles    Instret  Cyc/sample  Cyc/MAC  Checksum    Status      Pass\n");
    uart_puts("--------  --------  --------  ----------  -------  ----------  ----------  ----\n");
    uart_print_variant_row("tflm_ref", &result->ref, result->ref.pass);
    uart_print_variant_row("pulp_opt", &result->opt,
        result->opt.pass && (result->class_mismatches == 0u) &&
        (result->score_mismatches == 0u));

    uart_puts("\n\nExpected checksum: ");
    uart_put_u32_hex8(MNIST_FC_TEST_CHECKSUM);
    uart_puts("\nRef label matches: ");
    uart_put_u32_dec(result->ref.label_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nOpt label matches: ");
    uart_put_u32_dec(result->opt.label_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nRef expected-class matches: ");
    uart_put_u32_dec(result->ref.class_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nOpt expected-class matches: ");
    uart_put_u32_dec(result->opt.class_matches);
    uart_putc('/');
    uart_put_u32_dec(SAMPLE_COUNT);
    uart_puts("\nClass mismatches: ");
    uart_put_u32_dec(result->class_mismatches);
    uart_puts("\nScore mismatches: ");
    uart_put_u32_dec(result->score_mismatches);
    uart_puts("\nSpeedup: ");
    uart_put_speed(result->speedup_x100);
    uart_puts("\nLabels:           ");
    uart_print_labels();
    uart_puts("\nExpected classes: ");
    uart_print_expected_classes();
    uart_puts("\nTFLM classes:     ");
    uart_print_classes(tflm_classes);
    uart_puts("\nOpt classes:      ");
    uart_print_classes(opt_classes);
    uart_puts("\nSample0 TFLM:     ");
    uart_print_sample0_outputs(tflm_outputs[0]);
    uart_puts("\nSample0 opt:      ");
    uart_print_sample0_outputs(opt_outputs[0]);
    uart_puts("\nSample0 expected: ");
    uart_print_expected_sample0_outputs();
    uart_puts("\nOverall pass: ");
    uart_puts(result->pass ? "yes" : "no");
    uart_puts("\n\n");
}

int main(void)
{
    LED_REG = LED_STATUS_INIT;
    enable_perf_counters();

    LED_REG = LED_STATUS_SETUP;
    tflite::InitializeTarget();

    const tflite::Model *model = tflite::GetModel(g_mnist_fc_model_data);
    mnist_fc_result_t result = {};

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        result.ref.status = 0xe001u;
    } else {
        tflite::MicroMutableOpResolver<1> resolver;

        if (resolver.AddFullyConnected() != kTfLiteOk) {
            result.ref.status = 0xe002u;
        } else {
            tflite::MicroInterpreter interpreter(model, resolver,
                tensor_arena, sizeof(tensor_arena));

            result.ref.status = setup_tflm(&interpreter, model);
            if (result.ref.status == 0u) {
                measure_ref_vs_opt(&result);
            } else {
                result.opt.status = result.ref.status;
            }
        }
    }

    LED_REG = result.pass ? LED_STATUS_ALL_PASS : LED_STATUS_FAIL;

    while (1) {
        uart_print_report(&result);
        delay_cycles(REPORT_DELAY_CYCLES);
    }
}
