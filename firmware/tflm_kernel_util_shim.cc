#include <math.h>
#include <stdint.h>

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/kernel_util.h"

namespace tflite {

namespace {

TfLiteStatus GetQuantizedRange(TfLiteType type, int32_t *qmin, int32_t *qmax)
{
    switch (type) {
      case kTfLiteInt8:
        *qmin = -128;
        *qmax = 127;
        return kTfLiteOk;
      case kTfLiteUInt8:
        *qmin = 0;
        *qmax = 255;
        return kTfLiteOk;
      case kTfLiteInt16:
        *qmin = -32768;
        *qmax = 32767;
        return kTfLiteOk;
      default:
        return kTfLiteError;
    }
}

int32_t ClampToRange(int32_t value, int32_t qmin, int32_t qmax)
{
    if (value < qmin) {
        return qmin;
    }
    if (value > qmax) {
        return qmax;
    }
    return value;
}

int32_t QuantizeActivation(float value, const TfLiteTensor *output,
    int32_t qmin, int32_t qmax)
{
    const float scaled = value / output->params.scale;
    const int32_t quantized = output->params.zero_point +
        static_cast<int32_t>(roundf(scaled));
    return ClampToRange(quantized, qmin, qmax);
}

}  // namespace

TfLiteStatus GetQuantizedConvolutionMultipler(TfLiteContext *,
    const TfLiteTensor *input, const TfLiteTensor *filter,
    const TfLiteTensor *, TfLiteTensor *output, double *multiplier)
{
    if (input == nullptr || filter == nullptr || output == nullptr ||
        multiplier == nullptr || output->params.scale == 0.0f) {
        return kTfLiteError;
    }

    *multiplier = static_cast<double>(input->params.scale) *
        static_cast<double>(filter->params.scale) /
        static_cast<double>(output->params.scale);
    return kTfLiteOk;
}

TfLiteStatus GetQuantizedConvolutionMultipler(TfLiteContext *context,
    const TfLiteTensor *input, const TfLiteTensor *filter,
    TfLiteTensor *output, double *multiplier)
{
    return GetQuantizedConvolutionMultipler(context, input, filter, nullptr,
        output, multiplier);
}

TfLiteStatus CalculateActivationRangeQuantized(TfLiteContext *,
    TfLiteFusedActivation activation, TfLiteTensor *output, int32_t *act_min,
    int32_t *act_max)
{
    if (output == nullptr || act_min == nullptr || act_max == nullptr ||
        output->params.scale == 0.0f) {
        return kTfLiteError;
    }

    int32_t qmin;
    int32_t qmax;
    if (GetQuantizedRange(output->type, &qmin, &qmax) != kTfLiteOk) {
        return kTfLiteError;
    }

    switch (activation) {
      case kTfLiteActRelu:
        *act_min = QuantizeActivation(0.0f, output, qmin, qmax);
        *act_max = qmax;
        break;
      case kTfLiteActRelu6:
        *act_min = QuantizeActivation(0.0f, output, qmin, qmax);
        *act_max = QuantizeActivation(6.0f, output, qmin, qmax);
        break;
      case kTfLiteActReluN1To1:
        *act_min = QuantizeActivation(-1.0f, output, qmin, qmax);
        *act_max = QuantizeActivation(1.0f, output, qmin, qmax);
        break;
      case kTfLiteActNone:
      default:
        *act_min = qmin;
        *act_max = qmax;
        break;
    }

    return kTfLiteOk;
}

}  // namespace tflite
