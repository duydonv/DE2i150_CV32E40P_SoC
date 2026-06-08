# DE2i-150 + CV32E40P bring-up

A minimal clean-room bring-up of OpenHW's **CV32E40P** RISC-V core on the
Terasic **DE2i-150** (Cyclone IV GX `EP4CGX150DF31C7`) kit. This is the
sister project to the already-working PicoRV32 bring-up in
`../de2i_150_test` and reuses the same board integration style (pin map,
SDC, byte-lane BRAM, RISC-V GCC toolchain, `$readmemh` init).

The first milestone was **just blinking LEDs**. The current firmware now has
smoke, benchmark, tiny INT8 inference, and first TensorFlow Lite Micro bring-up
modes. These map the earlier gem5 AI-extension prototype onto the CORE-V
instructions already present in CV32E40P: `cv.mac`, `cv.sdotsp.b`, `cv.max`,
`cv.clipur`, `cv.lw` post-increment, and `cv.setup`.

Current status as of 2026-06-08:

- `COREV_PULP=1`, `FPU=0`, `COREV_CLUSTER=0`.
- `FPGA_TIMING_MODE=1` is enabled in the local core copy to close 50 MHz
  timing on Cyclone IV.
- The firmware is built as `rv32im_zicsr`, not `rv32imc`, so the hardware
  loop test stays word-aligned while benchmark code can read CSR counters.
- LEDG0 heartbeat, UART TX output, and the 3-row benchmark report have been
  verified on the DE2i-150 board.
- `tiny_ai` runs an exported 8x8 quadrant MLP and has been verified on the
  board with matching baseline/custom classes and 7.09x speedup.
- `tflm_hello` builds and runs the official TFLM `hello_world` int8 model on
  bare metal. The 128 KB BRAM image full-compiles to `.sof`, and the board
  UART run passes with checksum `0x3a357ded`.
- The MNIST FC `784 -> 32 -> 10` full-INT8 model now has fixed-vector
  firmware integration. The first reference-only board run passed, and the
  current `tflm_mnist_fc` image adds a `pulp_opt` path using `cv.sdotsp.b`
  dot4 with aligned `cv.lw`/`cv.setup` FC1x4 fast path plus model-derived
  per-channel requantization. The latest board run passes with `12.39x`
  validated speedup and `12.36x` inference-only speedup over TFLM reference on
  32 fixed MNIST vectors.
- `tflm_mnist_uart` now streams MNIST FC runtime inputs over UART. The runner
  reuses the fixed 32 vectors by default and also supports PGM image input via
  `--image`/`--images-dir`. Image mode converts each 28x28 grayscale pixel to
  `int8 = pixel - 128` on the host. The latest image-mode board run with
  `--images-dir --limit 10` passes `10/10`, label matches `10/10`, and shows
  `12.52x` aggregate speedup. A PySide6 GUI now reuses the same host protocol
  helpers for port selection, ping, preview, score bars, and one selected
  sample inference at a time.
- Questa RTL simulation now generates per-kernel VCD files for Quartus Power
  Analyzer; see `sim/README.md`.
- The sister gem5 prototype has been semantically aligned to this board
  mapping. gem5 still uses prototype custom-0 encodings and an O3 model, so its
  numbers are best treated as regression/sensitivity points rather than direct
  replacements for the board benchmark.

## Directory layout

```
de2i150_cv32e40p_soc/
├── rtl/
│   ├── core/                        <- CV32E40P SystemVerilog, copied from
│   │   ├── include/                     /home/duydonv/cv32e40p/rtl
│   │   │   ├── cv32e40p_pkg.sv
│   │   │   ├── cv32e40p_apu_core_pkg.sv
│   │   │   └── cv32e40p_fpu_pkg.sv
│   │   └── cv32e40p_*.sv
│   ├── fpga/
│   │   └── cv32e40p_clock_gate.sv   <- FPGA-safe replacement for the
│   │                                    latch-based sim/ASIC model
│   └── soc/
│       └── de2i150_cv32e40p_top.v   <- board wrapper: clocking, reset
│                                       sync, OBI <-> BRAM/UART/LED shim
├── firmware/
│   ├── ai_ops.h                      <- CORE-V/PULP .insn wrappers
│   ├── main.c                        <- PULP smoke test, LED pass/fail
│   ├── benchmark.c                   <- baseline-vs-CORE-V UART benchmark
│   ├── tiny_ai.c                      <- exported tiny int8 MLP, pre-TFLM
│   ├── tiny_ai_model.h                <- generated model weights/input data
│   ├── tflm_hello.cc                  <- first TFLM hello_world firmware
│   ├── tflm_port.cc                   <- TFLM target hooks for this SoC
│   ├── tflm_kernel_util_shim.cc       <- small generated-tree compatibility shim
│   ├── tflm_sources_minimal.txt       <- minimal TFLM source list
│   ├── mnist_fc/                       <- host-trained 784->32->10 INT8 TFLite
│   │                                    artifacts for next firmware milestone
│   ├── generate_tflm_tree.sh          <- regenerate ignored TFLM tree
│   ├── BENCHMARKS.md                  <- benchmark groups, UART output, LED codes
│   ├── TINY_AI.md                     <- tiny int8 MLP notes and board result
│   ├── TFLM.md                        <- TFLM bring-up notes
│   ├── perf.h                        <- CSR, LED, UART helpers
│   ├── start.s                       <- RV32 crt0
│   ├── sections.lds                  <- linker script (128 KB RAM at 0x0)
│   ├── split_hex.py                  <- turn firmware.bin into 4 byte-lanes
│   └── Makefile                      <- riscv64-unknown-elf, rv32im_zicsr
├── third_party/
│   └── tflm_tree/                    <- generated TFLM source tree, ignored by git
├── sim/
│   ├── power_tb.sv                    <- Questa testbench for power VCD windows
│   ├── run_power_vcd.sh               <- build/run one or all VCD scenarios
│   └── README.md                      <- VCD + Quartus Power Analyzer flow
├── de2i150_cv32e40p_top.sdc
├── PIN_ASSIGNMENTS.md
└── README.md
```

## What was (and was NOT) copied from upstream

From `/home/duydonv/cv32e40p/rtl`:

| Kept | Why |
|------|-----|
| `include/cv32e40p_pkg.sv` | package with enums used by almost every file |
| `include/cv32e40p_apu_core_pkg.sv` | APU-core interface widths, imported by `cv32e40p_top` |
| `include/cv32e40p_fpu_pkg.sv` | referenced by `cv32e40p_pkg`; still needed with FPU=0 |
| `cv32e40p_top.sv`, `cv32e40p_core.sv` | top + core |
| `cv32e40p_if_stage.sv`, `cv32e40p_id_stage.sv`, `cv32e40p_ex_stage.sv` | pipeline |
| `cv32e40p_load_store_unit.sv`, `cv32e40p_cs_registers.sv` | LSU + CSRs |
| `cv32e40p_register_file_ff.sv` | **flip-flop** register file, not the latch one |
| `cv32e40p_aligner.sv`, `cv32e40p_compressed_decoder.sv`, `cv32e40p_decoder.sv` | decode |
| `cv32e40p_fifo.sv`, `cv32e40p_prefetch_buffer.sv`, `cv32e40p_prefetch_controller.sv` | fetch FIFO |
| `cv32e40p_hwloop_regs.sv` | hardware-loop state used when `COREV_PULP=1` |
| `cv32e40p_mult.sv`, `cv32e40p_alu.sv`, `cv32e40p_alu_div.sv`, `cv32e40p_ff_one.sv`, `cv32e40p_popcnt.sv` | arith |
| `cv32e40p_apu_disp.sv` | referenced by core even with FPU=0 |
| `cv32e40p_controller.sv`, `cv32e40p_int_controller.sv` | control / IRQ |
| `cv32e40p_obi_interface.sv` | OBI request/response adapter |
| `cv32e40p_sleep_unit.sv` | instantiates `cv32e40p_clock_gate` |

Intentionally **excluded**:

- `rtl/cv32e40p_register_file_latch.sv` — uses `always_latch`, FPGA-hostile.
- `rtl/cv32e40p_fp_wrapper.sv` and everything under `rtl/vendor/pulp_platform_fpnew`
  — only wired in when `FPU=1`, which we disable.
- Everything under `bhv/` — assertions, tracers, **`cv32e40p_sim_clock_gate.sv`**
  (replaced by our FPGA shim).
- `example_tb/` — not needed for synthesis.

## FPGA gotchas, and how each is handled

### 1. Clock network is ASIC-style

CV32E40P's `cv32e40p_sleep_unit` and (when enabled) the FPU clock gating
both instantiate a module called `cv32e40p_clock_gate`. Upstream ships
only one implementation (`bhv/cv32e40p_sim_clock_gate.sv`) and the file
itself says in a comment:

> !!! cv32e40p_sim_clock_gate file is meant for simulation only !!!
> !!! It must not be used for ASIC synthesis                    !!!
> !!! It must not be used for FPGA synthesis                    !!!

Because it drives the clock through an `always_latch` and an AND gate,
Quartus would infer a transparent latch in the clock network — glitchy,
un-timeable, and un-routable on a global clock buffer.

**Fix**: `rtl/fpga/cv32e40p_clock_gate.sv` provides a pass-through
implementation. `clk_o` is tied directly to `clk_i`; the enable is
ignored with a `(* keep *)` dummy so it doesn't warn about no-driver.
The sleep unit still reports `core_sleep` via a register, so we still
see on LEDG1 whether the core has entered WFI, we just don't actually
gate the clock. That's the Quartus-recommended "clock enable on FF"
style rather than gating the clock net.

### 2. OBI bus on a board that has no OBI fabric

CV32E40P exposes two OBI-1.x style ports (`instr_*` and `data_*`) with
`req / gnt / rvalid / addr / (wdata, we, be) / rdata`. Plain Cyclone IV
BRAM can't speak OBI directly.

**Fix**: `rtl/soc/de2i150_cv32e40p_top.v` collapses the protocol for the
local BRAM and small MMIO devices:

- BRAM, LED, and UART status reads accept in one cycle.
- UART TX writes hold `data_gnt` low while the transmit shifter is busy.
- `rvalid` pulses one clock later for both reads and writes
- `data_req` is address-decoded into: (a) the 128 KB BRAM, (b) UART status
  at `0x0200_0000`, (c) UART TX byte write at `0x0200_0004`, (d) the LED
  MMIO register at `0x0300_0000`, or (e) a "grant-and-ignore" fall-through
  so the LSU never livelocks on a mis-typed pointer.

The 128 KB BRAM is split into four 8-bit byte-lanes with `(* ramstyle = "M9K" *)`
(same trick as the PicoRV32 project) so Quartus's inference does not fall
back to MLABs or logic. Instruction fetch uses port A (read only), data
LSU uses port B (R/W). Firmware is initialised from four
`firmware_byte<N>.hex` files via `$readmemh` at elaboration time.

### 3. Latches would kill FPGA timing

With the two exclusions listed above (`..._register_file_latch.sv`,
`cv32e40p_sim_clock_gate.sv`) there is no remaining `always_latch` in
the RTL that Quartus will see. You can double-check by running

```bash
grep -rn always_latch rtl/
```

after copying: the only remaining hit should be on a line inside a
`// !!!` comment banner, not in synthesisable RTL.

The SoC top (`de2i150_cv32e40p_top.v`) is written in plain Verilog-2001
specifically to avoid `always_comb` + `unique case` combinations that
Quartus Lite has historically reported as "does not infer purely
combinational logic".

### 4. Quartus Standard Edition and SystemVerilog

Quartus Prime Standard / Lite only supports **SystemVerilog-2005** at
most, so a few things matter:

1. Every `.sv` file must be registered as a SV source, not Verilog.
   Quartus does this automatically when the extension is `.sv`, but
   only if the global setting is enabled. Add this line to the `.qsf`
   once the project is created:

   ```tcl
   set_global_assignment -name VERILOG_INPUT_VERSION SYSTEMVERILOG_2005
   set_global_assignment -name SYSTEMVERILOG_INPUT_VERSION SYSTEMVERILOG_2005
   ```

2. Include search paths must be set, or `cv32e40p_pkg::*` won't resolve:

   ```tcl
   set_global_assignment -name SEARCH_PATH rtl/core/include
   ```

3. A lot of CV32E40P files use **SystemVerilog-2012** constructs that
   Quartus Lite rejects: multiple module-header `import`, `for (genvar i
   ...)`, bare `if`/`for` generate at module scope, `case ... inside`,
   `x inside {ranges}`, unsized enum literals. These have already been
   patched on the local copy in `rtl/core/`; see `fpga_patches/README.md`
   for the exact list. **Do not** copy CV32E40P files from upstream on
   top of `rtl/core/` without re-applying the patches.

### 5. PULP timing on Cyclone IV

Enabling `COREV_PULP=1` made the initial post-fit Fmax fall to roughly
37.6 MHz. The critical issue was the long direct producer-consumer path:

```text
EX result -> ID forwarding mux -> ID/EX operand register
```

The local core copy therefore adds an `FPGA_TIMING_MODE` parameter. With
`FPGA_TIMING_MODE=1`, the controller inserts one bubble for direct
ALU/MUL producer-consumer dependencies and disables the direct EX-to-ID
forwarding mux in that mode. This costs some IPC on back-to-back dependent
instructions, but closes the 50 MHz board clock.

The top-level SoC currently instantiates:

```verilog
cv32e40p_top #(
    .COREV_PULP      (1),
    .COREV_CLUSTER   (0),
    .FPU             (0),
    .FPU_ADDMUL_LAT  (0),
    .FPU_OTHERS_LAT  (0),
    .ZFINX           (0),
    .NUM_MHPMCOUNTERS(1),
    .FPGA_TIMING_MODE(1)
)
```

## How to create the Quartus project

You drive the GUI; the tool generates its own `.qpf` / `.qsf`. Only
source files and pin assignments are under our control.

1. **Open Quartus Prime 25.1 Std**.
2. **File → New Project Wizard**.
3. Set working directory to `/home/duydonv/de2i150_cv32e40p_soc`,
   project name `de2i150_cv32e40p_top`, top-level entity
   `de2i150_cv32e40p_top`.
4. On the "Add Files" page add (in this exact order so packages come
   first and are visible to later files):
   1. `rtl/core/include/cv32e40p_pkg.sv`
   2. `rtl/core/include/cv32e40p_apu_core_pkg.sv`
   3. `rtl/core/include/cv32e40p_fpu_pkg.sv`
   4. all remaining `rtl/core/*.sv`
   5. `rtl/fpga/cv32e40p_clock_gate.sv`
   6. `rtl/soc/de2i150_cv32e40p_top.v`
   7. `de2i150_cv32e40p_top.sdc`
5. Family/device: **Cyclone IV GX**, device **EP4CGX150DF31C7**.
6. Finish. Open **Assignments → Settings** and:
   - **Compiler Settings → Verilog HDL Input**: tick "SystemVerilog-2005".
   - **Compiler Settings → VHDL Input**: leave default.
   - **Libraries / Search Path**: add `rtl/core/include`.
7. **Assignments → Pin Planner** and enter every row of
   `PIN_ASSIGNMENTS.md`. Double-check I/O standard — `clock_50` is
   3.3-V LVTTL, UART is 3.3-V LVTTL, and the LED bank is 2.5-V.
8. **Processing → Start Compilation**. Fix only **errors**, not the
   thousands of info/warning messages the core emits (unused debug
   signals, inferred RAM, truncation warnings — all benign).
9. **Tools → Programmer** → add file `output_files/de2i150_cv32e40p_top.sof`
   → Start. Release `KEY0` after configuration completes.

## How to build the firmware

```bash
cd firmware
make                   # produces firmware.elf, firmware.bin,
                       #   firmware.hex, firmware_byte{0..3}.hex
```

The Makefile currently uses
`riscv64-unknown-elf-gcc -march=rv32im_zicsr -mabi=ilp32`. CV32E40P supports
compressed instructions, but this smoke firmware intentionally disables
the C extension so the `cv.setup` hardware-loop test remains word-aligned.

The stock toolchain does not know CORE-V mnemonics, so `firmware/ai_ops.h`
uses GNU assembler `.insn` directives for the PULP instructions. Once a
CORE-V-aware GCC is installed, these wrappers can be changed to mnemonic
inline assembly or builtins without changing the SoC. Always check the
result with `riscv64-unknown-elf-objdump -d firmware.elf` after changing
toolchains or wrappers.

Every time you edit firmware you must **re-run `make`** and then
**re-compile the Quartus project** (the `$readmemh` files are only
consumed at synthesis time — there is no runtime loader).

Firmware modes currently available:

```bash
make smoke       # default pass/fail smoke test on LEDR[7:0]
make benchmark   # baseline-vs-CORE-V/PULP performance benchmark
make tiny_ai     # exported tiny INT8 MLP, baseline vs CORE-V/PULP
make rx_smoke    # UART RX framed payload/checksum smoke test
make tflm_tiny_ai CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
                 # same tiny INT8 MLP as a TFLM reference model
make tflm_tiny_uart CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
                 # tiny TFLM model with UART RX runtime input frames
make tflm_mnist_fc CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
                 # MNIST 784->32->10 TFLM reference over fixed test vectors
```

The benchmark firmware measures `mcycle` and `minstret` for three groups:
`MAC + Clamp`, `Dot4_acc + Clamp`, and
`Dot4_acc + Clamp + p.lw + lp.setup`. Exact results are printed as a
fixed-width table over the board DB9 RS-232 UART at 115200 8N1.
`LEDR[7:0]` is only a compact status code; see `firmware/BENCHMARKS.md`.

The first TensorFlow Lite Micro mode needs a bare-metal C++ toolchain with
libstdc++ headers. The local Ubuntu `riscv64-unknown-elf-g++` package is not
enough for this. Use xPack GNU RISC-V Embedded GCC outside the repo; the
verified persistent location is under `/home/duydonv/tools`:

```bash
make tflm_hello CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
```

See `firmware/TFLM.md` for the toolchain download, generated TFLM source-tree
notes, board UART result, tiny MLP reference flow, and current linked size.

## Current AI/PULP instruction mapping

The board implementation is the current source of truth. The sister gem5
prototype has been updated to match these semantics, but exact opcode placement
is intentionally different: gem5 keeps prototype custom-0 encodings, while the
firmware maps the six operations to CORE-V/PULP hardware already in CV32E40P:

| Firmware operation | Current hardware mapping |
|--------------------|--------------------------|
| `ai_mac(acc, a, b)` | `cv.mac` via `.insn r 0x2b, 3, 0x48` |
| `ai_dot4_acc(acc, a, b)` | `cv.sdotsp.b` via `.insn r 0x7b, 1, 0x54` |
| `ai_relu(x)` | `cv.max x, x, x0` via `.insn r 0x2b, 3, 0x2d` |
| `ai_clamp(x, ub)` | `cv.clipur` via `.insn r 0x2b, 3, 0x3b` |
| `AI_PLW_U32(ptr, imm)` | `cv.lw rd, (rs1), imm` via `.insn i 0x0b, 2` |
| `AI_LP_SETUP_L0(count, body_bytes)` | `cv.setup L0, rs1, uimmL` via `.insn i 0x2b, 4` |

Two semantics are intentionally aligned to the real CORE-V hardware:

- `ai_clamp(x, ub)` clamps to `[0, ub]` with a register upper bound.
- `AI_PLW_U32` loads from the current base pointer and then post-increments
  the pointer by the signed immediate. For an offset stream, pre-bias the
  pointer before the loop.

For `cv.setup` encoded through raw `.insn`, the immediate is not byte-based:

```text
raw_imm = number_of_32_bit_body_instructions + 1
```

For example, a three-instruction loop body uses raw immediate `4`. The
`AI_LP_SETUP_L0(count, body_bytes)` macro handles this conversion.

## Expected behaviour after download

- **LEDG0** blinks at ~0.37 Hz (26-bit divider on 50 MHz). If this doesn't
  blink, the clock or reset is wrong — CPU cannot be at fault because
  the divider is independent of the core.
- **LEDG1** stays dark. It tracks `core_sleep`. If it lights, the core
  has executed `wfi`; the PULP smoke test never does.
- **LEDR[7:0]** blinks between `0xA5` and `0x00` if all smoke tests pass.
- **LEDR[7:0]** blinks between `0xE1`..`0xE6` and `0x00` if a smoke test fails:
  `E1=cv.mac`, `E2=cv.sdotsp.b`, `E3=cv.max/ReLU`, `E4=cv.clipur`,
  `E5=cv.lw post-increment`, `E6=cv.setup` hardware loop.
- With benchmark firmware, **UART TX** repeatedly prints the saved
  fixed-width report after the benchmark has run once. **LEDR[7:0]** shows
  `0x01` during init, `0x11` during the MAC/Clamp pair, `0x12` during the
  Dot4/Clamp pair, `0x13` during the Dot4/p.lw/lp.setup/Clamp pair,
  `0xA5` if all three groups pass, or a fail mask based at `0xE0`.
- With `tiny_ai` firmware, **UART TX** prints the tiny MLP baseline/custom
  classes, checksum, cycles/sample, and 8/8 accuracy.
- With `rx_smoke` firmware, **UART RX** accepts framed payloads at 115200 8N1
  and **UART TX** prints one `OK`/`ERR` line per received frame. See
  `firmware/UART_RX.md`.
- With `tflm_hello` firmware, **UART TX** prints the TFLM model size, tensor
  arena size, fixed int8 inputs, output vector, checksum, and runtime pass/fail
  status. The first milestone uses fixed firmware inputs; UART RX streaming is
  intentionally deferred.
- With `tflm_tiny_ai` firmware, **UART TX** now prints a fixed-sample
  ref-vs-opt report. `tflm_ref` uses official TFLM `FullyConnected`;
  `pulp_opt` uses `cv.sdotsp.b`, `cv.lw`, `cv.setup`, and `cv.clipur` while
  keeping TFLM requantization for bit-exact comparison. The last reference-only
  board run passed with checksum `0xc5f79430`, cycles `167327`, instret
  `118203`, and accuracy `8/8`; the new ref-vs-opt image builds and
  full-compiles. Its board run passes with ref cycles `167507`, opt cycles
  `29620`, checksum match `0xc5f79430`, zero class/score mismatches, and
  speedup `5.66x`.
- With `tflm_tiny_uart` firmware, **UART TX** prints one banner and then waits
  for framed **UART RX** input. It runs one runtime 64-byte sample through both
  `tflm_ref` and `pulp_opt`, then returns one `OK`/`ERR` line. It does not print
  a repeated report, so host scripts can use a strict request/response loop.
  The board runner passes with one ping plus 8 inference frames, zero
  ref-vs-opt score mismatches, and about `5.61x` speedup on the small model.
- With `tflm_mnist_fc` firmware, **UART TX** prints a fixed-vector MNIST FC
  ref-vs-opt report for the host-trained `784 -> 32 -> 10` INT8 model.
  `tflm_ref` uses official TFLM `FullyConnected`; `pulp_opt` reads the same
  TFLite flatbuffer weights/biases and uses `cv.sdotsp.b` dot4 with an aligned
  `cv.lw`/`cv.setup` FC1x4 fast path plus per-channel TFLite requantization.
  The fixed-vector checksum is
  `0x00cb95fc`, with 32/32 expected-class matches, 0 score mismatches, and
  31/32 label matches for the first 32 MNIST test vectors. The current board
  run passes with validated ref `11172961` cycles, opt `901789` cycles, and
  `12.39x` speedup. It also prints inference-only timing: ref `10817077`
  cycles, opt `874897` cycles, `12.36x` speedup. The MNIST optimized path still
  keeps scalar signed TFLite-compatible clamp/requantization; `cv.clipur`
  remains a follow-up after deciding whether to keep signed INT8 or regenerate
  an unsigned quantized model.
- With `tflm_mnist_uart` firmware, **UART RX** accepts one framed 784-byte
  quantized MNIST input tensor at a time. The host runner can send the fixed
  vector set, one generated PGM image, or all images under
  `firmware/mnist_fc/test_images_pgm/`. The generated image directory is
  ignored by git and can be recreated with
  `firmware/mnist_fc/export_test_images_pgm.py`.
- `firmware/tflm_mnist_uart_gui.py` is the current visual test client. It
  reuses the same protocol helpers as the terminal runner and currently runs
  one selected fixed vector or PGM image per button press, with preview,
  ref/opt class scores, cycle/speedup fields, true-label match, expected
  artifact match, and raw UART log.
- **LEDR[17:8]** stay dark to keep the board display readable.

## Current PULP synthesis status

With `COREV_PULP=1`, `FPU=0`, UART TX enabled, 128 KB local BRAM, and the
current TFLM firmware hex:

- Full Quartus compile passes and produces
  `output_files/de2i150_cv32e40p_top.sof`.
- Post-fit resources from `output_files/de2i150_cv32e40p_top.fit.summary`:
  13,109 logic elements, 2,614 registers, 2,097,152 memory bits, and 16
  embedded 9-bit multiplier elements.
- Timing is closed for the 50 MHz board clock with `FPGA_TIMING_MODE=1`.
  On the slow 1200 mV 85C model:
  - Worst setup slack: +0.337 ns
  - Worst hold slack: +0.374 ns

The latest full-compiled `tflm_tiny_ai` reference-only firmware uses the same
post-fit timing numbers. Its linked size was `text=47048 data=292 bss=8576
dec=55916`, and the embedded model flatbuffer is 2288 bytes. On the kit, the
UART run passed with checksum `0xc5f79430`, cycles `167327`, instret
`118203`, cycles/sample `20915`, cycles/MAC `19.22`, and accuracy `8/8`.

The current ref-vs-opt `tflm_tiny_ai` firmware builds with size `text=49880
data=292 bss=8616 dec=58788`. It full-compiles with the same post-fit resource
and timing numbers, producing `output_files/de2i150_cv32e40p_top.sof`. On the
kit, `tflm_ref` takes `167507` cycles and `pulp_opt` takes `29620` cycles,
for `5.66x` speedup with identical checksum `0xc5f79430` and `Overall pass:
yes`.

The first `tflm_mnist_fc` reference-only board run passed over UART:
checksum `0x00cb95fc`, expected-class matches `32/32`, score mismatches `0`,
label matches `31/32`, and `tflm_ref` cycles `11171144`. The current
ref-vs-opt firmware builds with size `text=107012 data=292 bss=14428
dec=121732`; `firmware.bin` is 107,308 bytes and fits in the 128 KB BRAM.
Full Quartus compile passed and produced
`output_files/de2i150_cv32e40p_top.sof`; post-fit resources are 13,381 logic
elements, 2,793 registers, 2,097,152 memory bits, and 16 embedded 9-bit
multiplier elements. Slow 1200 mV 85C timing closed with setup slack
`+0.256 ns` and hold slack `+0.375 ns`. The ref-vs-opt `.sof` assembler
checksum is `0x022DD42A`; UART report capture for this image passed:
validated `tflm_ref` cycles `11172961`, instret `7687470`; validated
`pulp_opt` cycles `901789`, instret `624902`; inference-only `tflm_ref`
cycles `10817077`, instret `7493516`; inference-only `pulp_opt` cycles
`874897`, instret `608100`. Both validated paths produced checksum
`0x00cb95fc`, expected-class `32/32`, score mismatches `0`, speedup `12.39x`,
inference-only speedup `12.36x`, and `Overall pass: yes`.

The QSF uses `PLACEMENT_EFFORT_MULTIPLIER=4.0`; with `3.0`, this design
placed successfully but missed slow-85C setup by about 0.2 ns.

## Roadmap

1. Smoke-test and benchmark firmware modes are now split via `make smoke`
   and `make benchmark`.
2. The FPGA benchmark now compares baseline C against the CORE-V/PULP
   wrappers and measures `mcycle` / `minstret` around three kernels.
3. Power analysis flow is present: run `sim/run_power_vcd.sh` to generate
   VCD activity files, then import each VCD into Quartus Power Analyzer with
   entity `de2i150_cv32e40p_top`.
4. Keep the gem5 prototype and this board implementation synchronized at the
   semantic/documentation level. Current synchronized points are register-bound
   clamp, CORE-V style post-increment load, and raw-immediate hardware-loop
   setup. Treat gem5 O3 results as modeling/sensitivity data, not as a
   replacement for measured kit results.
5. Install or build a CORE-V-aware toolchain so firmware can use CORE-V
   mnemonics or builtins instead of raw `.insn`.
6. `tiny_ai` is the pre-TFLM model-shaped reference. `tflm_hello` proves the
   C++ TFLM runtime can link, full-compile, and run on this SoC.
   `tflm_tiny_ai` and `tflm_tiny_uart` now cover small-model ref-vs-opt and
   UART runtime-input bring-up.
7. UART TX output is present at 115200 8N1. UART RX MMIO, standalone
   `rx_smoke`, and `tflm_tiny_uart` request/response inference are present.
   The larger MNIST FC model should reuse this protocol after adding the
   firmware target for `firmware/mnist_fc/mnist_fc_int8.tflite`.
8. Add a scratch LCD output mirror later if the board demo needs standalone
   display without a laptop.
9. Only after resource/performance/power data is available, decide whether to keep
   full `COREV_PULP=1` or turn PULP off and re-implement a smaller custom
   subset. The risky parts of a custom subset are `cv.setup` and `cv.lw`
   post-increment, because they touch PC/control-flow, LSU, and register
   writeback rather than only the ALU datapath.
