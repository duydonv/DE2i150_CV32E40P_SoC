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

Firmware status: `make tflm_mnist_fc` now embeds this flatbuffer and runs a
32-sample fixed-vector ref-vs-opt report on the DE2i-150. The `pulp_opt` path
uses the same TFLite model data rather than a duplicated hard-coded weight set.
It currently uses `cv.sdotsp.b` dot4 with an aligned `cv.lw`/`cv.setup` FC1x4
fast path plus TFLite-compatible per-channel requantization. If activation or
weight pointers are not 4-byte aligned, the firmware falls back to the earlier
scalar byte-pack path. Clamp-instruction tuning is still deferred because the
current model is signed INT8 and must keep TFLite-compatible signed activation
ranges.

Board status: the current ref-vs-opt image passes on the DE2i-150. The UART
capture reports validated-run `tflm_ref` cycles `11172961`, `pulp_opt` cycles
`901789`, speedup `12.39x`; inference-only `tflm_ref` cycles `10817077`,
`pulp_opt` cycles `874897`, speedup `12.36x`; checksum `0x00cb95fc` on both
paths, expected-class matches `32/32`, class mismatches `0`, score mismatches
`0`, and `Overall pass: yes`.

Runtime UART input status: `tflm_mnist_uart` uses the same model and optimized
kernel, but receives one quantized 784-byte input tensor per frame. The host
script `../tflm_mnist_uart_runner.py` sends these 32 vectors over UART and
checks the response against `mnist_fc_test_vectors.h`. This is the terminal
test path before adding arbitrary image input or a GUI.

For image-mode UART testing, generated PGM images can be placed under
`test_images_pgm/`. That directory is ignored by git because it is reproducible
from the MNIST test split. The UART runner can send either one image with
`--image` or all generated images with `--images-dir`, converting each pixel
with `int8 = pixel - 128`.

Regenerate the default test image set with:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
python3 mnist_fc/export_test_images_pgm.py
```

Current image-mode UART board result: `--images-dir --limit 10` passes `10/10`
generated PGM samples from test indices `32..41`, label matches `10/10`, and
the aggregate speedup is `12.52x`.

## Files

| File | Meaning |
|---|---|
| `train_mnist_fc.py` | Reproducible train/quantize/verify script |
| `mnist_fc_float.keras` | Trained float Keras model |
| `mnist_fc_int8.tflite` | Full INT8 TFLite flatbuffer, source of truth for firmware |
| `mnist_fc_model_data.{cc,h}` | C byte array wrapping the `.tflite` model |
| `mnist_fc_test_vectors.h` | First 32 quantized MNIST test samples and expected INT8 outputs |
| `mnist_fc_metadata.json` | Accuracy, checksum, quantization, and training metadata |
| `export_test_images_pgm.py` | Recreate ignored PGM test images for UART image-mode testing |
