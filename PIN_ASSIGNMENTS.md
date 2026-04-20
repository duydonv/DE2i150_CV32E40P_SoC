# Pin assignments — DE2i-150 + CV32E40P bring-up

Use these assignments in Quartus Pin Planner for the first clean build.
They are a direct subset of the values already verified against the
Terasic DE2i-150 user manual in the earlier `de2i_150_test` project, so
they can be trusted without re-checking the board schematic.

## Top-level port list of `de2i150_cv32e40p_top`

| Top-level port | Board signal | FPGA pin | I/O standard |
| --- | --- | --- | --- |
| `clock_50` | `CLOCK_50` | `PIN_AJ16` | `3.3-V LVTTL` |
| `reset_n` | `KEY0` | `PIN_AA26` | `2.5-V` |
| `ledr[0]` | `LEDR0` | `PIN_T23` | `2.5-V` |
| `ledr[1]` | `LEDR1` | `PIN_T24` | `2.5-V` |
| `ledr[2]` | `LEDR2` | `PIN_V27` | `2.5-V` |
| `ledr[3]` | `LEDR3` | `PIN_W25` | `2.5-V` |
| `ledr[4]` | `LEDR4` | `PIN_T21` | `2.5-V` |
| `ledr[5]` | `LEDR5` | `PIN_T26` | `2.5-V` |
| `ledr[6]` | `LEDR6` | `PIN_R25` | `2.5-V` |
| `ledr[7]` | `LEDR7` | `PIN_T27` | `2.5-V` |
| `ledr[8]` | `LEDR8` | `PIN_P25` | `2.5-V` |
| `ledr[9]` | `LEDR9` | `PIN_R24` | `2.5-V` |
| `ledr[10]` | `LEDR10` | `PIN_P21` | `2.5-V` |
| `ledr[11]` | `LEDR11` | `PIN_N24` | `2.5-V` |
| `ledr[12]` | `LEDR12` | `PIN_N21` | `2.5-V` |
| `ledr[13]` | `LEDR13` | `PIN_M25` | `2.5-V` |
| `ledr[14]` | `LEDR14` | `PIN_K24` | `2.5-V` |
| `ledr[15]` | `LEDR15` | `PIN_L25` | `2.5-V` |
| `ledr[16]` | `LEDR16` | `PIN_M21` | `2.5-V` |
| `ledr[17]` | `LEDR17` | `PIN_M22` | `2.5-V` |
| `ledg[0]` | `LEDG0` heartbeat | `PIN_AA25` | `2.5-V` |
| `ledg[1]` | `LEDG1` core_sleep | `PIN_AB25` | `2.5-V` |

## Notes

- Bring-up firmware drives only `ledr[7:0]`. `ledr[17:8]` are tied low in RTL,
  so they must still be assigned to valid pins with a valid I/O standard —
  otherwise Quartus will refuse to place the design.
- `ledg[0]` blinks at ~0.37 Hz from the hardware heartbeat divider. If it
  does not blink, the clock network or reset is wrong — CPU cannot be the
  culprit because the divider is independent of the core.
- `ledg[1]` reflects `core_sleep`. With `FPU=0` and `COREV_CLUSTER=0`, it
  will only go high if firmware executes `wfi`. The LED-shift demo never
  does, so expect it to stay dark.
- If you later add UART / LCD, copy the remaining rows from
  `../de2i_150_test/PIN_ASSIGNMENTS.md` — pin list was already validated
  against the user manual in that project.
