# FPGA benchmark firmware

This firmware mode compares plain RV32IM C kernels against the CORE-V/PULP
instruction wrappers already used by the smoke test. It is intended for the
real DE2i-150 kit. Results are printed over the board DB9 RS-232 UART at
115200 8N1.

Build the smoke firmware, as before:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make smoke
```

Build the benchmark firmware:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make benchmark
```

After either build, re-run Quartus compilation because the top-level BRAM is
initialized from `firmware/firmware_byte{0..3}.hex` at synthesis/elaboration
time.

## Benchmark groups

### Benchmark 1: MAC + clamp

- Baseline: RV32IM C loop using `mul` + add, then C clamp.
- Custom: `ai_mac`, then `ai_clamp`.
- Work size: `512 outputs * 32 taps = 16384 MAC operations`.
- Upper clamp: `2047`.

This isolates the MAC arithmetic plus output clamp:

```text
MAC, Clamp
```

### Benchmark 2: dot4_acc + clamp

- Baseline: C loop with two normal loads, four signed int8 multiplies, add,
  then C clamp.
- Custom: normal C loads and normal C loop control, but the packed INT8 dot
  product uses `cv.sdotsp.b`, then `ai_clamp`.
- Work size: `128 outputs * 64 packed words = 8192 dot4_acc operations`
  or `32768 signed int8 lane multiplies`.
- Upper clamp: `255`.

This isolates the packed INT8 ALU/MUL operation without post-increment loads
or hardware loop setup:

```text
Dot4_acc, Clamp
```

### Benchmark 3: streaming dot4 + clamp

- Baseline: same scalar C dot4/clamp kernel as benchmark 2.
- Custom: hardware loop `cv.setup`, post-increment loads `cv.lw`,
  `cv.sdotsp.b`, then `ai_clamp`.
- Work size: `128 outputs * 64 packed words = 8192 dot4_acc operations`
  or `32768 signed int8 lane multiplies`.
- Upper clamp: `255`.

This measures the full streaming INT8 path:

```text
Dot4_acc, Clamp, p.lw, lp.setup
```

All benchmark pairs compute checksums. A pair passes only if the baseline and
custom checksums match.

## Operation counts

The UART `Ops` column counts only the main arithmetic operation for the
benchmark row: MAC for benchmark 1 and dot4_acc for benchmarks 2/3. Helper
operations are listed separately here.

| Benchmark | Main arithmetic ops | Clamp ops | Loads | Hardware-loop setup |
|---|---:|---:|---:|---:|
| `mac_clamp` | `512 * 32 = 16384` MAC | 512 | normal C loads | none |
| `dot4_acc_clamp` | `128 * 64 = 8192` dot4_acc | 128 | `128 * 64 * 2 = 16384` normal word loads | none |
| `dot4_plw_lp_clamp` | `128 * 64 = 8192` dot4_acc | 128 | `128 * 64 * 2 = 16384` `cv.lw` post-increment loads | 128 `cv.setup` |

Each dot4_acc operation is equivalent to four signed int8 lane multiplies,
so `8192` dot4_acc operations represent `32768` int8 lane multiplies.

## UART output

Connect the DE2i-150 DB9 connector through a real RS-232 adapter, then open
the serial port as `115200 8N1`, no flow control. Example:

```bash
picocom -b 115200 /dev/ttyUSB0
```

The benchmark runs once after reset, stores the results in RAM, then prints
the same fixed-width report repeatedly. This makes it safe to open the
terminal slightly after the FPGA starts; the benchmark is not re-run between
reports.

Latest observed board output:

```text
DE2i-150 CV32E40P benchmark
UART: 115200 8N1
Clock: 50000000 Hz

ID  Benchmark                 Pass     Ops    Base cyc    Cust cyc    Speed    Base ins    Cust ins  Checksum
--  ------------------------  ----  ------  ----------  ----------  -------  ----------  ----------  ---------------------
 1  mac_clamp                 yes    16384      281412      263212    1.07x      226532      208924             0x06535320
 2  dot4_acc_clamp            yes     8192      496830       78516    6.33x      396974       52388             0x9587070e
 3  dot4_plw_lp_clamp         yes     8192      496830       37938   13.10x      396974       27811             0x9587070e
```

`Speed` is `baseline_cycles / custom_cycles`. `Pass=yes` means baseline and
custom checksums match. If a checksum comparison fails, the checksum column
prints `baseline/custom` so the mismatch is visible.

## LED status

UART carries the exact numbers. `LEDR[7:0]` now only shows a compact state:

| LEDR[7:0] | Meaning |
|---|---|
| `0x01` | initialization |
| `0x11` | running MAC/Clamp pair |
| `0x12` | running Dot4/Clamp pair |
| `0x13` | running Dot4/p.lw/lp.setup/Clamp pair |
| `0xA5` | all checksum comparisons passed |
| `0xE1` | benchmark 1 checksum mismatch |
| `0xE2` | benchmark 2 checksum mismatch |
| `0xE4` | benchmark 3 checksum mismatch |
| `0xE7` | all checksum comparisons mismatched |

`LEDR[17:8]` are driven low in RTL to keep the board display readable.
