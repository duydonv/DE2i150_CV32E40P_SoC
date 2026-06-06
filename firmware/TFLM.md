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
reference. The standalone UART RX smoke test now verifies byte transport
separately; TFLM runtime input streaming should be added after the larger FC
model shape is stable, so I/O protocol work does not obscure model/runtime
changes.

Build it with:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_tiny_ai CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

The build first compiles `generate_tflm_tiny_ai_model.cc` for the host, then
regenerates `tflm_tiny_ai_model_data.cc/.h` from `tiny_ai_model.h`. This keeps
the `.tflite` model source of truth aligned with the existing tiny C reference
without requiring TensorFlow on the host.

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
`status=0x00000001`, and manual `picocom` frame injection also passed. The next
use of RX should be runtime input streaming for the larger FC model, while this
small smoke firmware remains a regression test for the serial link itself.

## Next Step

The ref-vs-opt `tflm_tiny_ai` firmware builds, full-compiles, and passes on the
board, and UART RX is now verified independently. The next main milestone should
move to a larger fully-connected model, e.g. `784 -> 32 -> 10`, then connect the
already-tested RX protocol to runtime input streaming. Kernel tuning should wait
until that larger shape is stable, because the tiny `64 -> 16 -> 4` model
over-amplifies setup/requantization/checksum overhead compared with the real
workload.
