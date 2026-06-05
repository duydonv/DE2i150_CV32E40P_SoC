#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffer_builder.h"
#include "flatbuffers/default_allocator.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tiny_ai_model.h"

namespace {

constexpr float kInputScale = 1.0f;
constexpr int64_t kInputZeroPoint = 0;
constexpr float kWeightScale = 1.0f;
constexpr int64_t kWeightZeroPoint = 0;
constexpr float kFc1BiasScale = kInputScale * kWeightScale;
constexpr float kHiddenScale = 512.0f;
constexpr int64_t kHiddenZeroPoint = 0;
constexpr float kFc2BiasScale = kHiddenScale * kWeightScale;
constexpr float kOutputScale = kHiddenScale * 128.0f;
constexpr int64_t kOutputZeroPoint = -128;

template <typename T>
void AppendScalar(std::vector<uint8_t>* bytes, T value)
{
    for (size_t i = 0; i < sizeof(T); ++i) {
        bytes->push_back(static_cast<uint8_t>(
            (static_cast<uint64_t>(value) >> (8u * i)) & 0xffu));
    }
}

std::vector<uint8_t> Int8Bytes(const int8_t* data, size_t count)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        bytes.push_back(static_cast<uint8_t>(data[i]));
    }
    return bytes;
}

std::vector<uint8_t> Int32Bytes(const int32_t* data, size_t count)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(count * sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) {
        AppendScalar<int32_t>(&bytes, data[i]);
    }
    return bytes;
}

flatbuffers::Offset<tflite::QuantizationParameters>
MakeQuant(flatbuffers::FlatBufferBuilder* builder, float scale,
    int64_t zero_point)
{
    std::vector<float> scales = {scale};
    std::vector<int64_t> zero_points = {zero_point};
    return tflite::CreateQuantizationParametersDirect(
        *builder, nullptr, nullptr, &scales, &zero_points);
}

flatbuffers::Offset<tflite::Tensor>
MakeTensor(flatbuffers::FlatBufferBuilder* builder,
    const std::vector<int32_t>& shape, tflite::TensorType type,
    uint32_t buffer, const char* name, float scale, int64_t zero_point)
{
    auto quant = MakeQuant(builder, scale, zero_point);
    return tflite::CreateTensorDirect(
        *builder, &shape, type, buffer, name, quant, false, 0, nullptr, true);
}

flatbuffers::Offset<tflite::Buffer>
MakeBuffer(flatbuffers::FlatBufferBuilder* builder,
    const std::vector<uint8_t>& data)
{
    return tflite::CreateBufferDirect(*builder, &data);
}

flatbuffers::Offset<tflite::Operator>
MakeFullyConnected(flatbuffers::FlatBufferBuilder* builder,
    const std::vector<int32_t>& inputs, const std::vector<int32_t>& outputs,
    tflite::ActivationFunctionType activation)
{
    auto options = tflite::CreateFullyConnectedOptions(
        *builder, activation, tflite::FullyConnectedOptionsWeightsFormat_DEFAULT,
        false, false, tflite::TensorType_INT32);
    return tflite::CreateOperatorDirect(*builder, 0, &inputs, &outputs,
        tflite::BuiltinOptions_FullyConnectedOptions, options.Union());
}

std::vector<uint8_t> BuildModel()
{
    flatbuffers::DefaultAllocator allocator;
    flatbuffers::FlatBufferBuilder builder(4096, &allocator);

    const std::vector<uint8_t> empty;
    auto buffers = builder.CreateVector(std::vector<flatbuffers::Offset<tflite::Buffer>>{
        MakeBuffer(&builder, empty),
        MakeBuffer(&builder, Int8Bytes(&tiny_ai_fc1_weight[0][0],
            TINY_AI_HIDDEN_DIM * TINY_AI_INPUT_DIM)),
        MakeBuffer(&builder, Int32Bytes(tiny_ai_fc1_bias, TINY_AI_HIDDEN_DIM)),
        MakeBuffer(&builder, Int8Bytes(&tiny_ai_fc2_weight[0][0],
            TINY_AI_OUTPUT_DIM * TINY_AI_HIDDEN_DIM)),
        MakeBuffer(&builder, Int32Bytes(tiny_ai_fc2_bias, TINY_AI_OUTPUT_DIM)),
    });

    auto tensors = builder.CreateVector(std::vector<flatbuffers::Offset<tflite::Tensor>>{
        MakeTensor(&builder, {1, static_cast<int32_t>(TINY_AI_INPUT_DIM)},
            tflite::TensorType_INT8, 0, "input", kInputScale,
            kInputZeroPoint),
        MakeTensor(&builder, {static_cast<int32_t>(TINY_AI_HIDDEN_DIM),
                              static_cast<int32_t>(TINY_AI_INPUT_DIM)},
            tflite::TensorType_INT8, 1, "fc1_weight", kWeightScale,
            kWeightZeroPoint),
        MakeTensor(&builder, {static_cast<int32_t>(TINY_AI_HIDDEN_DIM)},
            tflite::TensorType_INT32, 2, "fc1_bias", kFc1BiasScale, 0),
        MakeTensor(&builder, {1, static_cast<int32_t>(TINY_AI_HIDDEN_DIM)},
            tflite::TensorType_INT8, 0, "hidden", kHiddenScale,
            kHiddenZeroPoint),
        MakeTensor(&builder, {static_cast<int32_t>(TINY_AI_OUTPUT_DIM),
                              static_cast<int32_t>(TINY_AI_HIDDEN_DIM)},
            tflite::TensorType_INT8, 3, "fc2_weight", kWeightScale,
            kWeightZeroPoint),
        MakeTensor(&builder, {static_cast<int32_t>(TINY_AI_OUTPUT_DIM)},
            tflite::TensorType_INT32, 4, "fc2_bias", kFc2BiasScale, 0),
        MakeTensor(&builder, {1, static_cast<int32_t>(TINY_AI_OUTPUT_DIM)},
            tflite::TensorType_INT8, 0, "output", kOutputScale,
            kOutputZeroPoint),
    });

    auto operators = builder.CreateVector(std::vector<flatbuffers::Offset<tflite::Operator>>{
        MakeFullyConnected(&builder, {0, 1, 2}, {3},
            tflite::ActivationFunctionType_RELU),
        MakeFullyConnected(&builder, {3, 4, 5}, {6},
            tflite::ActivationFunctionType_NONE),
    });

    std::vector<int32_t> graph_inputs = {0};
    std::vector<int32_t> graph_outputs = {6};
    auto subgraph_fixed = tflite::CreateSubGraph(
        builder, tensors, builder.CreateVector(graph_inputs),
        builder.CreateVector(graph_outputs), operators,
        builder.CreateString("tiny_ai_main"));

    auto op_codes = builder.CreateVector(std::vector<flatbuffers::Offset<tflite::OperatorCode>>{
        tflite::CreateOperatorCode(builder,
            static_cast<int8_t>(tflite::BuiltinOperator_FULLY_CONNECTED), 0, 1,
            tflite::BuiltinOperator_FULLY_CONNECTED),
    });

    auto subgraphs = builder.CreateVector(
        std::vector<flatbuffers::Offset<tflite::SubGraph>>{subgraph_fixed});

    auto model = tflite::CreateModel(builder, 3, op_codes, subgraphs,
        builder.CreateString("DE2i-150 tiny_ai int8 MLP"), buffers);
    tflite::FinishModelBuffer(builder, model);

    flatbuffers::Verifier verifier(builder.GetBufferPointer(),
        builder.GetSize());
    if (!tflite::VerifyModelBuffer(verifier)) {
        std::fprintf(stderr, "generated TFLM tiny_ai model failed verifier\n");
        std::exit(1);
    }

    return std::vector<uint8_t>(builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize());
}

void WriteHeader(const std::string& path, size_t model_size)
{
    std::ofstream out(path);
    out << "#ifndef TFLM_TINY_AI_MODEL_DATA_H\n"
        << "#define TFLM_TINY_AI_MODEL_DATA_H\n\n"
        << "constexpr unsigned int g_tflm_tiny_ai_model_data_size = "
        << model_size << ";\n"
        << "extern const unsigned char g_tflm_tiny_ai_model_data[];\n\n"
        << "#endif\n";
}

void WriteSource(const std::string& path, const std::vector<uint8_t>& model)
{
    std::ofstream out(path);
    out << "#include \"tflm_tiny_ai_model_data.h\"\n\n";
    out << "alignas(16) const unsigned char g_tflm_tiny_ai_model_data[] = {";
    for (size_t i = 0; i < model.size(); ++i) {
        if ((i % 12u) == 0u) {
            out << "\n    ";
        } else {
            out << ' ';
        }
        out << "0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(model[i]);
        if (i + 1u != model.size()) {
            out << ',';
        }
    }
    out << "\n};\n";
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string out_dir = argc > 1 ? argv[1] : ".";
    const auto model = BuildModel();

    WriteHeader(out_dir + "/tflm_tiny_ai_model_data.h", model.size());
    WriteSource(out_dir + "/tflm_tiny_ai_model_data.cc", model);

    std::printf("wrote TFLM tiny_ai model: %zu bytes\n", model.size());
    return 0;
}
