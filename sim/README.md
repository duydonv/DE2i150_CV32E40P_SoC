# Power VCD simulation

This directory contains the Questa RTL simulation flow used to generate VCD
activity files for Quartus Power Analyzer.

Verified status as of 2026-05-23:

- Questa `vlog` compile: 0 errors, only benign SystemVerilog port-kind
  warnings.
- `dot4_plw_lp_clamp_custom` simulation: checksum PASS and VCD contains
  real signal activity when `vsim` is run with `-voptargs=+acc`.
- Quartus Power Analyzer accepts generated VCDs when the Power Input File
  `Entity` is set to `de2i150_cv32e40p_top`.

The normal FPGA benchmark firmware still runs all three benchmark pairs and
prints the UART table. For power activity, the script rebuilds the firmware
with `POWER_SIM` enabled so each simulation run executes exactly one kernel
variant, starts VCD dumping at the kernel entry LED marker, then stops after
the checksum pass/fail LED marker.

## Scenarios

```text
mac_clamp_baseline
mac_clamp_custom
dot4_acc_clamp_baseline
dot4_acc_clamp_custom
dot4_plw_lp_clamp_baseline
dot4_plw_lp_clamp_custom
```

The streaming baseline uses the same scalar dot4/clamp kernel as
`dot4_acc_clamp_baseline`; it is emitted as a separate VCD so the files align
with the benchmark table.

## Generate VCD files

From the project root:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc
./sim/run_power_vcd.sh --list
```

Run one scenario first:

```bash
./sim/run_power_vcd.sh dot4_plw_lp_clamp_custom
```

Run all scenarios:

```bash
./sim/run_power_vcd.sh
```

Outputs are written to:

```text
sim/vcd/<scenario>.vcd
```

Temporary Questa files are written to:

```text
sim/build/questa/
```

The script runs `vsim` with `-voptargs=+acc` so `$dumpvars` can see the RTL
signals needed for VCD activity. Without that option, Questa may optimize the
design so aggressively that the VCD only contains empty scopes.

Full-design VCD files are large. A short custom dot4 streaming run can already
be hundreds of MB, and the scalar baseline VCDs are larger because they run for
more cycles. Run one scenario at a time if disk space is tight.

By default the script restores the normal `make benchmark` firmware after the
power simulations finish, so the `firmware/firmware_byte*.hex` files are again
the UART benchmark firmware for Quartus compilation.

If `vlog` or `vsim` are not in `PATH`, source the Questa/Quartus environment
first, or override the command names:

```bash
VLOG=/path/to/vlog VSIM=/path/to/vsim ./sim/run_power_vcd.sh mac_clamp_custom
```

Increase the simulation timeout if needed:

```bash
TIMEOUT_CYCLES=4000000 ./sim/run_power_vcd.sh
```

## View waveforms in Questa

To inspect a specific scenario interactively, first compile the RTL/testbench:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc
./sim/run_power_vcd.sh --compile-only
```

Then build the POWER_SIM firmware for one scenario. For example,
`POWER_SIM_BENCH=3` and `POWER_SIM_VARIANT=1` selects
`dot4_plw_lp_clamp_custom`:

```bash
make -C firmware PROGRAM=benchmark EXTRA_CFLAGS="-DPOWER_SIM -DPOWER_SIM_BENCH=3 -DPOWER_SIM_VARIANT=1 -Wno-unused-function" clean all
```

Open Questa GUI:

```bash
cd sim/build/questa
vsim -voptargs=+acc work.tb_power
```

Useful starter waves:

```tcl
add wave /tb_power/clock_50
add wave /tb_power/reset_n
add wave /tb_power/ledr
add wave /tb_power/u_dut/instr_req
add wave /tb_power/u_dut/instr_addr
add wave /tb_power/u_dut/instr_rvalid
add wave /tb_power/u_dut/data_req
add wave /tb_power/u_dut/data_we
add wave /tb_power/u_dut/data_addr
add wave /tb_power/u_dut/data_wdata
add wave /tb_power/u_dut/data_rdata
run -all
```

Expected LED marker sequence in waveform:

```text
0x01 -> init
0x40 -> power activity window starts
0xA5 -> checksum pass, simulation ends
0xEF -> checksum fail
```

After manual POWER_SIM builds, restore the normal UART benchmark firmware
before compiling Quartus for the kit:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc
make -C firmware benchmark
```

## Use VCD in Quartus Power Analyzer

1. Build the Quartus project normally first so a fitted design exists.
2. Open `Processing > Power Analyzer Tool`.
3. Enable input activity files and add one generated VCD file.
4. Set the Power Input File `Entity` to the fitted Quartus top-level entity:

```text
de2i150_cv32e40p_top
```

Do not add `tb_power` to the Quartus project. `tb_power` is only the Questa
testbench hierarchy; Quartus Power Analyzer only accepts entities from the
compiled FPGA design here.

5. Run Power Analyzer and save the report for that scenario.
6. Repeat for the other VCD files you want to compare.

For reporting, compare both average power and energy:

```text
energy = average_power * runtime_seconds
runtime_seconds = cycles / 50000000
```

Average power alone can be misleading: a custom kernel may have similar or
slightly higher instantaneous power, but much lower energy because it finishes
in fewer cycles.
