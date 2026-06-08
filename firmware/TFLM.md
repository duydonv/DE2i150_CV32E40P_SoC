# TensorFlow Lite Micro bring-up

This is the first TFLM milestone for the DE2i-150 CV32E40P board. It runs the
official TFLM `hello_world` int8 model with a single `FullyConnected` operator.
The goal is to prove that the C++ TFLM runtime, model flatbuffer, tensor arena,
and UART reporting all work on the bare-metal SoC before replacing kernels with
the CORE-V/PULP INT8 path.

## Toolchain

The Ubuntu `riscv64-unknown-elf-g++` package on this machine does not provide
the C++ standard headers needed by TFLM. Use an embedded RISC-V toolchain with
newlib and libstdc++ support. The current persistent setup uses xPack GNU
RISC-V Embedded GCC 14.2.0.

Do not keep the toolchain in `/tmp` for long-term work. `/tmp` was only used
for the first bring-up and may disappear after reboot. Prefer a persistent
location under the home directory. This path is now installed and verified:

```bash
mkdir -p /home/duydonv/tools
cd /home/duydonv/tools
curl -L https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v14.2.0-3/xpack-riscv-none-elf-gcc-14.2.0-3-linux-x64.tar.gz -o xpack-riscv-none-elf-gcc-14.2.0-3-linux-x64.tar.gz
mkdir -p xpack-riscv-none-elf-gcc
tar -xzf xpack-riscv-none-elf-gcc-14.2.0-3-linux-x64.tar.gz -C xpack-riscv-none-elf-gcc
```

The SHA-256 observed for that archive was:

```text
f574415b63f12b09bdd3475223ab492a465d23810646c90c13a4c3b676c83503
```

After installing the toolchain there, build the firmware with:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_hello CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

If the toolchain is installed somewhere else, only change the `CROSS=` prefix.

The first bring-up used this temporary path before the persistent install was
documented:

```text
/tmp/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

## Upstream TFLM Source

The upstream TFLM clone is also kept outside the SoC repo:

```text
/home/duydonv/src/tflite-micro
```

The clone was verified clean at commit:

```text
ac1fae3
```

Normal rebuilds of the current `tflm_hello` firmware use the generated tree at
`third_party/tflm_tree`. That tree is intentionally ignored by git and can be
regenerated from this upstream clone when needed.

## Repository vs External Dependencies

The board-specific TFLM integration files are committed to this repository:

- `firmware/tflm_hello.cc`: board firmware using the TFLM hello_world model.
- `firmware/tflm_tiny_ai.cc`: board firmware using the tiny MLP model through
  reference TFLM `FullyConnected`.
- `firmware/generate_tflm_tiny_ai_model.cc`: host-side flatbuffer generator for
  the tiny MLP model. It uses the existing `tiny_ai_model.h` weights/data and
  the vendored TFLM schema, so it does not require TensorFlow Python or `flatc`.
- `firmware/tflm_tiny_ai_model_data.{cc,h}`: generated tiny MLP flatbuffer
  embedded as a C++ byte array.
- `firmware/mnist_fc/`: host-trained MNIST `784 -> 32 -> 10` full-INT8 TFLite
  artifacts for the next firmware milestone. It includes the reproducible
  TensorFlow train/quantize/verify script, `.tflite`, C byte array, metadata,
  fixed test vectors, and the PGM image export script.
- `firmware/tflm_mnist_uart.cc`: MNIST FC request/response UART runtime-input
  firmware.
- `firmware/tflm_mnist_uart_runner.py` and `firmware/mnist_uart_protocol.py`:
  reusable host UART protocol, fixed-vector, and PGM image-mode runner code.
- `firmware/tflm_port.cc`: target hooks for logging/time/setup.
- `firmware/tflm_kernel_util_shim.cc`: small compatibility shim needed by the
  generated tree used here.
- `firmware/tflm_sources_minimal.txt`: source list used to build the narrow
  TFLM library for hello_world + FullyConnected.
- `firmware/generate_tflm_tree.sh`: regenerates the local TFLM generated tree.

The following are intentionally not committed:

- The xPack compiler toolchain. Keep this under `/home/duydonv/tools`, not in
  the SoC repo.
- The original upstream TFLM clone. It is only needed when regenerating or
  updating `third_party/tflm_tree`.
- The generated TFLM tree `third_party/tflm_tree`.
- Generated MNIST PGM images under `firmware/mnist_fc/test_images_pgm`.
- Download archives and temporary generated trees.

For normal rebuilds of the current `tflm_hello` milestone, the repo plus a
persistent C++ RISC-V toolchain is enough only if `third_party/tflm_tree`
already exists locally. On a fresh checkout, regenerate it once:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
./generate_tflm_tree.sh
```

The script uses:

- `TFLM_SRC=/home/duydonv/src/tflite-micro`
- expected upstream commit `ac1fae3`
- output `third_party/tflm_tree`

Manual regeneration is equivalent to:

```bash
cd /home/duydonv/src/tflite-micro
git checkout ac1fae3
python3 tensorflow/lite/micro/tools/project_generation/create_tflm_tree.py -e hello_world /tmp/tflm-tree-riscv
```

Then replace `/home/duydonv/de2i150_cv32e40p_soc/third_party/tflm_tree` with
that generated output. Do this only when the TFLM version or selected
example/model support changes.

## Firmware

`tflm_hello.cc` performs:

- model schema check
- `MicroMutableOpResolver<1>` with `AddFullyConnected()`
- `MicroInterpreter::AllocateTensors()`
- one warm-up invoke over four fixed int8 inputs
- one measured invoke loop over the same four fixed inputs

The UART report prints cycle count, instruction count, checksum, status, input
values, and output int8 values. `Pass=yes` means the TFLM runtime path returned
`kTfLiteOk` for setup and all measured invokes.

The vendored tree in `third_party/tflm_tree` was generated from upstream
TensorFlow Lite Micro with `create_tflm_tree.py -e hello_world`. The current
TFLM source list is intentionally narrow and lives in
`tflm_sources_minimal.txt`. It is sized for `hello_world` plus
`FullyConnected`. The tiny MLP reference model also uses only
`FullyConnected`. Add op source files only when the selected model actually
needs them.

## Tiny MLP Ref-vs-Opt Model

The next TFLM milestone after `hello_world` is `tflm_tiny_ai`. It runs the same
8x8 quadrant MLP shape as `tiny_ai.c`:

```text
64 int8 inputs -> FullyConnected 16 hidden -> ReLU -> FullyConnected 4 outputs
```

This firmware now contains two fixed-sample paths:

- `tflm_ref`: official TFLM reference `FullyConnected`.
- `pulp_opt`: local optimized MLP body using `cv.sdotsp.b`, `cv.lw`,
  `cv.setup`, and `cv.clipur`.

The optimized path keeps TFLM's `MultiplyByQuantizedMultiplier` requantization
step so the first target is bit-exact score/checksum matching against the TFLM
reference. The standalone UART RX smoke test verifies byte transport
separately; `tflm_tiny_uart` then adds runtime input streaming to this small
model first. This keeps protocol/debug flow stable before changing to the
larger FC model shape.

Build it with:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_tiny_ai CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

The build first compiles `generate_tflm_tiny_ai_model.cc` for the host, then
regenerates `tflm_tiny_ai_model_data.cc/.h` from `tiny_ai_model.h`. This keeps
the `.tflite` model source of truth aligned with the existing tiny C reference
without requiring TensorFlow on the host.

## Tiny MLP UART Runtime Input Mode

`tflm_tiny_uart` is the first TFLM mode that consumes input over UART RX. It
uses the same small model as `tflm_tiny_ai` and is intentionally separate from
the fixed-sample benchmark firmware:

- `tflm_tiny_ai`: runs the built-in 8 samples once, then repeatedly prints the
  saved report.
- `tflm_tiny_uart`: prints one banner, waits for one RX frame, runs one input
  sample, prints one response, then waits for the next frame.

This avoids mixing the old continuous TX report with a request/response host
runner. The host must send only one frame at a time and wait for `OK`/`ERR`
before sending the next frame.

Build it with:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_tiny_uart CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

Current host build result:

```text
Model bytes: 2288
Firmware size: text=49536 data=292 bss=8668 dec=58496
```

After rebuilding firmware, re-run Quartus compilation and program the `.sof`.
Then run:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
python3 tflm_tiny_uart_runner.py /dev/ttyUSB0
```

Protocol:

```text
55 aa cmd len_lo len_hi payload checksum_le32
```

Commands:

| Command | Payload | Meaning |
|---:|---|---|
| `0x10` | 0 bytes | ping, returns one `OK` line |
| `0x11` | 64 bytes | run one int8 input sample through TFLM ref and `pulp_opt` |

An inference response looks like:

```text
OK seq=2 cmd=0x11 len=64 pass=yes ref_cls=0 opt_cls=0 mismatches=0 ...
```

`pass=yes` requires matching TFLM/optimized class, score vector, and checksum.
The default runner sends the same 8 quadrant samples used by the fixed firmware
and checks the expected class labels on the host.

Current board result:

```text
ping: OK seq=1 cmd=0x10
infer: 8/8 frames pass, classes 0 0 1 1 2 2 3 3
ref cycles: 20975..20976
opt cycles: 3738..3739
speedup: 5.61x
rx_status: 0x00000001
```

LED status while running:

| LEDR[7:0] | Meaning |
|---|---|
| `0x80` | init |
| `0x81` | TFLM setup |
| `0x82` | ready/waiting for the next RX frame |
| `0x83` | receiving/checking a frame |
| `0x84` | running TFLM reference inference |
| `0x85` | running optimized inference |
| `0xa5` | last handled frame passed |
| `0xef` | last handled frame failed, or setup failed |

After the runner finishes successfully, the board normally shows `0x82` because
the firmware has returned to the idle state and is waiting for another frame.

The TFLM quantization is set up to preserve the intended fixed-point shape:

- input and weight tensors use scale `1`, zero point `0`.
- the hidden tensor uses scale `512`, matching the first fixed shift.
- the output tensor uses scale `65536` and zero point `-128`; firmware reports
  scores as `output_int8 + 128`, so the visible scores are in `0..255`.

Expected reference behavior is classification accuracy `8/8` with labels:

```text
0 0 1 1 2 2 3 3
```

The board UART reference checksum is `0xc5f79430`. The optimized path is
expected to match this checksum exactly. Do not treat it as the same checksum as
`tiny_ai.c`, because TFLM uses its own standard quantized rounding path.

Current firmware build status for `tflm_tiny_ai` ref-vs-opt:

```text
Model bytes: 2288
Firmware size: text=49880 data=292 bss=8616 dec=58788
Optimized kernel: cv.sdotsp.b + cv.lw + cv.setup + cv.clipur
Fixed-sample target checksum: 0xc5f79430
SOF: output_files/de2i150_cv32e40p_top.sof
Quartus: 0 errors, 84 warnings
Post-fit resources: 13109 logic elements, 2614 registers, 2097152 memory bits, 16 DSP elements
Timing slow 1200mV 85C: setup slack +0.337 ns, hold slack +0.374 ns
Board UART run: pass, checksum 0xc5f79430, speedup 5.66x
```

Ref-vs-opt board UART result captured on the kit:

```text
DE2i-150 CV32E40P TFLM tiny INT8 MLP ref-vs-opt
UART: 115200 8N1
Clock: 50000000 Hz
Model: 8x8 quadrant MLP int8 64 -> 16 -> 4
Model bytes: 2288
Tensor arena: 8192 bytes
Samples: 8
INT8 MACs: 8704
Custom dot4 ops: 2176
Optimized kernel: cv.sdotsp.b + cv.lw + cv.setup + cv.clipur

Variant   Cycles    Instret  Cyc/sample  Cyc/MAC  Checksum    Status      Pass
--------  --------  --------  ----------  -------  ----------  ----------  ----
tflm_ref    167507    118343       20938   19.24  0xc5f79430  0x00000000  yes
pulp_opt     29620     20864        3702    3.40  0xc5f79430  0x00000000  yes

Expected labels: 0 0 1 1 2 2 3 3
TFLM classes:    0 0 1 1 2 2 3 3
Opt classes:     0 0 1 1 2 2 3 3
Ref accuracy: 8/8
Opt accuracy: 8/8
Class mismatches: 0
Score mismatches: 0
Speedup: 5.66x
Overall pass: yes
Sample0 scores TFLM: 40 0 0 0
Sample0 scores opt:  40 0 0 0
```

Last full-compile/board result for the reference-only `tflm_tiny_ai` image:

```text
Model bytes: 2288
Firmware size: text=47048 data=292 bss=8576 dec=55916
SOF: output_files/de2i150_cv32e40p_top.sof
Quartus: 0 errors, 84 warnings
Post-fit resources: 13109 logic elements, 2614 registers, 2097152 memory bits, 16 DSP elements
Timing slow 1200mV 85C: setup slack +0.337 ns, hold slack +0.374 ns
```

Reference-only board UART result captured on the kit:

```text
DE2i-150 CV32E40P TFLM tiny INT8 MLP reference
UART: 115200 8N1
Clock: 50000000 Hz
Model: 8x8 quadrant MLP int8 64 -> 16 -> 4
Model bytes: 2288
Tensor arena: 8192 bytes
Samples: 8
INT8 MACs: 8704

Cycles    Instret  Cyc/sample  Cyc/MAC  Checksum    Status      Pass
--------  --------  ----------  -------  ----------  ----------  ----
  167327    118203       20915   19.22  0xc5f79430  0x00000000  yes

Expected labels: 0 0 1 1 2 2 3 3
TFLM classes:    0 0 1 1 2 2 3 3
Accuracy: 8/8
Sample0 scores: 40 0 0 0
```

## Memory

TFLM does not fit in the original 32 KB local BRAM. The current linked
`tflm_hello` image is about 50 KB total:

```text
text=46484 data=292 bss=4444 dec=51220
```

The SoC BRAM and linker script are now set to 128 KB:

- `rtl/soc/de2i150_cv32e40p_top.v`: `BRAM_AW_WORDS = 15`
- `firmware/sections.lds`: `LENGTH = 0x20000`
- `firmware/split_hex.py`: `WORDS = 32768`

After building TFLM firmware, run Quartus compilation again so the enlarged
BRAM and new `firmware_byte{0..3}.hex` initialization are picked up.

The current full compile with `tflm_hello` firmware completed successfully:

```text
SOF: output_files/de2i150_cv32e40p_top.sof
Quartus: 0 errors, 84 warnings
Post-fit: 13109 logic elements, 2614 registers, 2097152 memory bits, 16 DSP elements
Timing slow 1200mV 85C: setup slack +0.337 ns, hold slack +0.374 ns
```

## Board Result

Latest observed UART output from the DE2i-150 kit:

```text
DE2i-150 CV32E40P TFLM hello_world int8
UART: 115200 8N1
Clock: 50000000 Hz
Model: hello_world int8 FullyConnected
Model bytes: 2704
Tensor arena: 4096 bytes
Invokes: 4

Cycles    Instret  Cyc/invoke  Checksum    Status      Pass
--------  --------  ----------  ----------  ----------  ----
   45149     32117       11287  0x3a357ded  0x00000000  yes

Inputs int8:  -96 -63 -34 0
Outputs int8: 89 125 93 4
Cycles/input x100: 11287.25
```

`Pass=yes` means the runtime path completed successfully: model schema check,
op resolver setup, tensor allocation, warm-up invoke, and all measured invokes
returned `kTfLiteOk`. This is a TFLM runtime bring-up test, not an accuracy
benchmark for a real AI task.

## UART RX

The first TFLM milestones intentionally used fixed firmware inputs, matching
the earlier benchmark bring-up style. UART RX has now been added at the SoC
MMIO level and verified separately with `rx_smoke`, not yet wired into TFLM
inference:

- status/TX/RX MMIO: `0x0200_0000`, `0x0200_0004`, `0x0200_0008`
- RX status bits: `RX_VALID`, `RX_OVERRUN`, `RX_FRAME_ERROR`
- RX FIFO depth: 16 bytes, implemented in logic
- firmware smoke mode: `make rx_smoke`
- host smoke script: `firmware/rx_smoke_runner.py`
- manual terminal test: `picocom` hex-write frames

The scripted soak test passed payload lengths `1,16,64,255,512` repeatedly with
`status=0x00000001`, and manual `picocom` frame injection also passed. On top of
that, `tflm_tiny_uart` now wires the same framed protocol into the small TFLM
model for runtime input validation before moving to the larger FC model.

## Next Step

The ref-vs-opt `tflm_tiny_ai` firmware builds, full-compiles, and passes on the
board, UART RX is verified independently, and `tflm_tiny_uart` now passes
runtime-input board testing on the small model. Host artifacts for the larger
MNIST fully-connected model are now prepared under `firmware/mnist_fc/`:

```text
Model: 784 -> 32 + ReLU -> 10
INT8 TFLite accuracy: 96.28% (9628/10000)
INT8 checksum: 0x7c33a8dc
Test-vector checksum: 0x00cb95fc
Model bytes: 28368
Input quant: scale 0.003921568859368563, zero_point -128
Output quant: scale 0.26571762561798096, zero_point 37
```

The MNIST FC firmware target is `tflm_mnist_fc`. It embeds
`mnist_fc_model_data.{cc,h}` and runs 32 fixed MNIST vectors in one report.
`tflm_ref` uses official TFLM `FullyConnected`; `pulp_opt` reads the same
flatbuffer weights, biases, scales, and zero-points after `AllocateTensors()`,
then evaluates both FC layers with `cv.sdotsp.b` dot4 and per-channel TFLite
requantization. There is no separate hard-coded weight set for the optimized
path.

The current board image reads constant weight/bias tensors directly from the
TFLite flatbuffer `Buffer` objects. This avoids the TFLM Micro `GetTensor()`
constant-buffer issue that produced setup status `0xe036` and zeroed
ref-vs-opt outputs in an earlier image.

The current MNIST optimized kernel uses an aligned FC1x4 stream fast path
around the dot4 body. FC1 processes four hidden outputs per tile: the hardware
loop body loads one activation word, loads four weight words, and updates four
independent accumulators with `cv.sdotsp.b`. If alignment is not suitable, the
firmware falls back to the earlier scalar `dot4_i8()` path. FC2 remains on the
scalar aligned `cv.lw`/`cv.setup` dot4 path because its total MAC count is much
smaller.

Build:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_mnist_fc CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

Runtime UART input uses the separate target `tflm_mnist_uart`. It keeps the
same flatbuffer model and optimized FC path, but waits for one framed input at
a time instead of running the fixed vectors internally. The host payload is the
already-quantized MNIST input tensor: 784 raw bytes, where each byte is the
two's-complement representation of the model `int8` value.

```text
55 aa cmd len_lo len_hi payload checksum_le32
cmd 0x10: ping, len 0
cmd 0x11: infer one MNIST vector, len 784
```

Build:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_mnist_uart CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

After Quartus compile and programming, verify the UART data path with the 32
fixed MNIST vectors from `mnist_fc_test_vectors.h`:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
python3 tflm_mnist_uart_runner.py /dev/ttyUSB0
```

The runner sends one ping and then 32 `0x11` frames. It checks that the board
returns `pass=yes`, zero ref-vs-opt score mismatches, and the same expected
class and score vector as the fixed-vector firmware artifacts. This runner is
also the intended base for a later GUI client.

The same runner can send exported PGM test images. These images are generated
artifacts and are ignored by git:

```bash
python3 mnist_fc/export_test_images_pgm.py
python3 tflm_mnist_uart_runner.py /dev/ttyUSB0 --images-dir
python3 tflm_mnist_uart_runner.py /dev/ttyUSB0 --image mnist_fc/test_images_pgm/mnist_test_00032_label3.pgm
```

In image mode, the host converts each 28x28 grayscale pixel to the model input
byte with `int8 = pixel - 128`. By default, image-mode pass/fail checks the
UART path and ref-vs-opt equality; it reports label matches separately because
the model can legitimately misclassify some MNIST test images. Add
`--require-label-match` when a label mismatch should fail the run.

Current MNIST UART image-mode board result: the first 10 generated PGM images
from MNIST test indices `32..41` pass with ref-vs-opt score mismatches `0`,
label matches `10/10`, ref cycles `350615..350622`, opt cycles
`27999..28014`, and aggregate speedup `12.52x`.

The first GUI wrapper for the same protocol is `tflm_mnist_uart_gui.py`. It is
a PySide6 + pyqtgraph frontend that reuses `mnist_uart_protocol.py` for serial
frames, PGM loading, quantization, and response parsing. It provides port/baud
selection, ping, sample list, image preview, one-sample inference, score bars,
timing fields, and a raw UART log.

Set up and run:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc
python3 -m venv .venv_gui
.venv_gui/bin/python -m pip install -r firmware/gui_requirements.txt
.venv_gui/bin/python firmware/tflm_mnist_uart_gui.py
```

The GUI reports true-label matches separately from expected-artifact matches.
This matters for the 32 fixed vectors: they are expected to match the stored
TFLM reference score vectors exactly, while one of them is a model
misclassification against the original MNIST label.

Current ref-vs-opt build/full-compile/program/board status:

```text
Firmware size: text=107012 data=292 bss=14428 dec=121732
firmware.bin: 107308 bytes
SOF: output_files/de2i150_cv32e40p_top.sof
Quartus flow: successful
Post-fit resources: 13381 logic elements, 2793 registers, 2097152 memory bits, 16 DSP elements
Timing slow 1200mV 85C: setup slack +0.256 ns, hold slack +0.375 ns
SOF assembler checksum: 0x022DD42A
```

The previous reference-only image was captured on the board and passed:
checksum `0x00cb95fc`, label matches `31/32`, expected-class matches `32/32`,
score mismatches `0`, cycles `11171144`, instret `7711381`.

The current ref-vs-opt image was captured on the board and passed. The
validated run includes fixed-vector input handling, inference, argmax,
checksum, and validation bookkeeping:

```text
tflm_ref: cycles 11172961, instret 7687470, cyc/sample 349155, cyc/MAC 13.74,
          checksum 0x00cb95fc, status 0x00000000, pass yes
pulp_opt: cycles 901789, instret 624902, cyc/sample 28180, cyc/MAC 1.11,
          checksum 0x00cb95fc, status 0x00000000, pass yes
Labels: 31/32 for both paths
Expected-class matches: 32/32 for both paths
Class mismatches: 0
Score mismatches: 0
Speedup: 12.39x
Overall pass: yes
```

The UART report also prints a separate inference-only pass. For `tflm_ref`,
the measured interval only covers `Invoke()` after the fixed-vector input has
already been copied into the input tensor. For `pulp_opt`, the measured
interval only covers the custom inference call:

```text
tflm_ref: cycles 10817077, instret 7493516, cyc/sample 338033, cyc/MAC 13.30
pulp_opt: cycles 874897, instret 608100, cyc/sample 27340, cyc/MAC 1.08
Inference-only speedup: 12.36x
```

The next firmware step is either to reuse the existing request/response
protocol with 784-byte input frames or to evaluate whether an unsigned
quantized model is worth generating so `cv.clipur` can be used cleanly in the
activation clamp path.
