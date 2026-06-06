# MNIST FC INT8 Artifacts

This directory contains the host-generated artifacts for the next firmware
milestone:

```text
784 int8 inputs -> FullyConnected 32 + ReLU -> FullyConnected 10 outputs
```

The model was trained on MNIST with TensorFlow/Keras, converted to a full INT8
TFLite flatbuffer, and verified again with the TFLite Interpreter.

## Recreate

The current host environment used a temporary TensorFlow CPU venv:

```bash
python3 -m venv /tmp/mnist_fc_tf_venv
/tmp/mnist_fc_tf_venv/bin/python -m pip install --upgrade pip setuptools wheel tensorflow-cpu
```

Regenerate artifacts:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware/mnist_fc
env TF_ENABLE_ONEDNN_OPTS=0 KERAS_HOME=/tmp/mnist_fc_keras \
  /tmp/mnist_fc_tf_venv/bin/python train_mnist_fc.py \
  --out-dir . \
  --epochs 20 \
  --batch-size 128 \
  --representative-count 500 \
  --verify-limit 10000 \
  --vector-count 32 \
  --seed 1234
```

## Current Result

```text
Float accuracy: 96.30%
INT8 accuracy: 96.28% (9628/10000)
INT8 checksum: 0x7c33a8dc
Test-vector checksum: 0x00cb95fc
TFLite model size: 28368 bytes
```

Quantization:

```text
Input:  int8, shape [1, 784], scale 0.003921568859368563, zero_point -128
Output: int8, shape [1, 10],  scale 0.26571762561798096,  zero_point 37
```

The TFLite graph contains two INT8 `FULLY_CONNECTED` ops. Input, weights,
hidden activation, and output are `int8`; biases are `int32`.

## Files

| File | Meaning |
|---|---|
| `train_mnist_fc.py` | Reproducible train/quantize/verify script |
| `mnist_fc_float.keras` | Trained float Keras model |
| `mnist_fc_int8.tflite` | Full INT8 TFLite flatbuffer, source of truth for firmware |
| `mnist_fc_model_data.{cc,h}` | C byte array wrapping the `.tflite` model |
| `mnist_fc_test_vectors.h` | First 32 quantized MNIST test samples and expected INT8 outputs |
| `mnist_fc_metadata.json` | Accuracy, checksum, quantization, and training metadata |

