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
ac1fae36
```

Normal rebuilds of the current `tflm_hello` firmware use the generated tree at
`third_party/tflm_tree`. That tree is intentionally ignored by git and can be
regenerated from this upstream clone when needed.

## Repository vs External Dependencies

The board-specific TFLM integration files are committed to this repository:

- `firmware/tflm_hello.cc`: board firmware using the TFLM hello_world model.
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
- expected upstream commit `ac1fae36`
- output `third_party/tflm_tree`

Manual regeneration is equivalent to:

```bash
cd /home/duydonv/src/tflite-micro
git checkout ac1fae36
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
`FullyConnected`. Add op source files only when the selected model actually
needs them.

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

The first TFLM milestone does not need UART RX. Inputs are fixed in firmware,
matching the earlier benchmark bring-up style. UART RX should be added later
only when runtime input streaming or a loader is needed; doing it now would mix
I/O protocol debugging with TFLM runtime debugging.

## Next Step

The next useful milestone is a tiny int8 MLP `.tflite` model shaped like
`tiny_ai.c`, still using reference TFLM kernels, before optimizing the TFLM
`FullyConnected` kernel with `cv.sdotsp.b`, `cv.lw`, and `cv.setup`.
