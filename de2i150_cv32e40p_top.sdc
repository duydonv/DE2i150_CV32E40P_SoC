create_clock -name clock_50 -period 20.000 [get_ports {clock_50}]

# Asynchronous reset input: treat as a false path rather than try to meet
# timing against clock_50. The reset synchroniser inside the top handles
# async-assert / sync-deassert.
set_false_path -from [get_ports {reset_n}] -to [all_registers]

# LED outputs: allow Quartus to pick an output delay that fits any slow
# consumer; we only need them metastability-free at human time scales.
set_false_path -from [all_registers] -to [get_ports {ledr[*] ledg[*]}]
