# DE2i-150 + CV32E40P bring-up

A minimal clean-room bring-up of OpenHW's **CV32E40P** RISC-V core on the
Terasic **DE2i-150** (Cyclone IV GX `EP4CGX150DF31C7`) kit. This is the
sister project to the already-working PicoRV32 bring-up in
`../de2i_150_test` and reuses the same board integration style (pin map,
SDC, byte-lane BRAM, RISC-V GCC toolchain, `$readmemh` init).

The first milestone is **just blinking LEDs** — no UART, no LCD, no
interrupts, no debug module. Once this is stable, we extend it with the
custom AI / quantisation instructions (MAC, dot4.acc, ReLU, clamp,
lp.setup, p.lw) that the core already has hooks for.

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
│                                       sync, OBI <-> BRAM shim, LED MMIO
├── firmware/
│   ├── main.c                        <- LED shift (writes 0x0300_0000)
│   ├── start.s                       <- RV32 crt0
│   ├── sections.lds                  <- linker script (32 KB RAM at 0x0)
│   ├── split_hex.py                  <- turn firmware.bin into 4 byte-lanes
│   └── Makefile                      <- riscv64-unknown-elf, rv32imc
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
| `cv32e40p_hwloop_regs.sv` | even with COREV_PULP=0, file is referenced |
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

## The four gotchas you flagged, and how each is handled

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

**Fix**: `rtl/soc/de2i150_cv32e40p_top.v` collapses the protocol for a
single-cycle BRAM:

- `gnt = req` (always accept)
- `rvalid` pulses one clock later for both reads and writes
- `data_req` is address-decoded into: (a) the 32 KB BRAM, (b) the LED
  MMIO register at `0x0300_0000`, or (c) a "grant-and-ignore" fall-through
  so the LSU never livelocks on a mis-typed pointer.

The BRAM is split into four 8-bit byte-lanes with `(* ramstyle = "M9K" *)`
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
   3.3-V LVTTL, everything on the LED bank is 2.5-V.
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

The Makefile uses `riscv64-unknown-elf-gcc -march=rv32imc -mabi=ilp32`.
`rv32imc` matches what CV32E40P implements out of the box (I + M + C,
no F, no custom PULP). When we later enable the custom AI extension we
will switch to a custom `-march` via a GCC multi-arch or `.insn r`
macros; the infrastructure in this directory does not change.

Every time you edit firmware you must **re-run `make`** and then
**re-compile the Quartus project** (the `$readmemh` files are only
consumed at synthesis time — there is no runtime loader).

## Expected behaviour after first download

- **LEDG0** blinks at ~0.37 Hz (26-bit divider on 50 MHz). If this doesn't
  blink, the clock or reset is wrong — CPU cannot be at fault because
  the divider is independent of the core.
- **LEDG1** stays dark. It tracks `core_sleep`. If it lights, the core
  has executed `wfi`; the LED-shift demo never does.
- **LEDR[7:0]** shifts one lit LED from `LEDR0` to `LEDR7` and wraps.
  Period ~0.3 s (from the `delay(1500000)` loop).
- **LEDR[17:8]** stay dark. Reserved for later demos.

## Roadmap

1. Add a simple UART peripheral copied from the PicoRV32 project so we
   can print heartbeats and observe state over `/dev/ttyUSB0` at 115200
   8N1.
2. Add a scratch LCD output mirror, also from the PicoRV32 project.
3. Enable `COREV_PULP=1` in the core, then validate `lp.setup`,
   `p.lw`, MAC/dot-product from firmware. This is the reason we picked
   CV32E40P in the first place.
4. Bolt on the custom AI quantisation instructions (MAC, dot4.acc,
   ReLU, clamp) via the APU interface or as extra decoder entries,
   depending on pipeline pressure.
5. Bring up a tiny INT8 test kernel and profile cycle-accurately in
   Questa before pushing a synthesis build.
