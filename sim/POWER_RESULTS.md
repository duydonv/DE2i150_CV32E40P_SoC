# Quartus Power Analyzer results

This file records per-scenario Quartus Power Analyzer results for the RTL VCD
power flow. All values come from `output_files/de2i150_cv32e40p_top.pow.rpt`
after selecting the listed VCD as the Power Analyzer input file.

The estimates currently have low confidence because most internal activity is
still vectorless-estimated. Use the numbers primarily for same-design,
same-flow scenario comparison.

## Measurement setup

| Field | Value |
|---|---|
| Quartus version | 25.1std.0 Build 1129 10/21/2025 SC Lite Edition |
| Top entity | `de2i150_cv32e40p_top` |
| Device | `EP4CGX150DF31C7` |
| Power models | Final |
| Device power characteristics | Typical |
| Ambient temperature | 25.0 C |
| VCD entity | `de2i150_cv32e40p_top` |
| VCD glitch filtering | On |
| Vectorless estimation | On |

## Scenario results

| Scenario | VCD | VCD size | Report time | Total thermal (mW) | Core dynamic (mW) | Core static (mW) | I/O (mW) | Avg toggle rate (Mtr/s) | Confidence | VCD unknown | VCD toggle | Simulation mapped | Vectorless mapped |
|---|---|---:|---|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|
| `dot4_acc_clamp_baseline` | `sim/vcd/dot4_plw_lp_clamp_baseline.vcd` | 4.6 GiB | 2026-05-24 11:40:31 +0700 | 370.96 | 237.78 | 119.85 | 13.34 | 25.152 | Low | 2.5% | 32.0% | 1077 signals (6.2%) | 16152 signals (93.7%) |
| `dot4_acc_clamp_custom` | `sim/vcd/dot4_acc_clamp_custom.vcd` | 586.4 MiB | 2026-05-24 11:47:40 +0700 | 362.82 | 229.67 | 119.82 | 13.34 | 24.232 | Low | 13.8% | 41.4% | 1077 signals (6.2%) | 16152 signals (93.7%) |
| `dot4_plw_lp_clamp_baseline` | `sim/vcd/dot4_plw_lp_clamp_baseline.vcd` | 4.6 GiB | 2026-05-24 11:40:31 +0700 | 370.96 | 237.78 | 119.85 | 13.34 | 25.152 | Low | 2.5% | 32.0% | 1077 signals (6.2%) | 16152 signals (93.7%) |
| `dot4_plw_lp_clamp_custom` | `sim/vcd/dot4_plw_lp_clamp_custom.vcd` | 346.3 MiB | 2026-05-24 11:21:07 +0700 | 373.25 | 240.06 | 119.85 | 13.34 | 24.696 | Low | 24.9% | 41.3% | 1077 signals (6.2%) | 16152 signals (93.7%) |
| `mac_clamp_baseline` | `sim/vcd/mac_clamp_baseline.vcd` | 2.4 GiB | 2026-05-24 11:59:35 +0700 | 365.45 | 232.28 | 119.83 | 13.34 | 24.718 | Low | 0.3% | 30.8% | 1077 signals (6.2%) | 16152 signals (93.7%) |
| `mac_clamp_custom` | `sim/vcd/mac_clamp_custom.vcd` | 2.1 GiB | 2026-05-24 11:57:16 +0700 | 359.88 | 226.73 | 119.81 | 13.34 | 23.657 | Low | 0.3% | 33.6% | 1077 signals (6.2%) | 16152 signals (93.7%) |

## Notes

- `Simulation mapped` is the number of signals whose toggle/static probability
  came from the VCD, not from vectorless estimation.
- `Vectorless mapped` is the remaining fitted-design activity estimated by
  Quartus Power Analyzer.
- Keep one scenario per Power Analyzer run; multiple VCDs in one run combine
  activity sources and make scenario comparison ambiguous.
- `dot4_acc_clamp_baseline` and `dot4_plw_lp_clamp_baseline` use the same
  scalar baseline kernel, so the same baseline VCD/report is recorded for both
  comparison groups.
