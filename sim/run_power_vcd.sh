#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/sim/build/questa"
VCD_DIR="$ROOT_DIR/sim/vcd"

VLIB_BIN="${VLIB:-vlib}"
VLOG_BIN="${VLOG:-vlog}"
VSIM_BIN="${VSIM:-vsim}"
MAKE_BIN="${MAKE:-make}"
TIMEOUT_CYCLES="${TIMEOUT_CYCLES:-2000000}"

RESTORE_FIRMWARE=1
COMPILE_ONLY=0
BUILT_POWER_FIRMWARE=0
REQUESTED=()

SCENARIOS=(
    "mac_clamp_baseline|benchmark|1|0"
    "mac_clamp_custom|benchmark|1|1"
    "dot4_acc_clamp_baseline|benchmark|2|0"
    "dot4_acc_clamp_custom|benchmark|2|1"
    "dot4_plw_lp_clamp_baseline|benchmark|3|0"
    "dot4_plw_lp_clamp_custom|benchmark|3|1"
    "tiny_ai_baseline|tiny_ai|0|0"
    "tiny_ai_custom|tiny_ai|0|1"
)

RTL_FILES=(
    "$ROOT_DIR/rtl/core/include/cv32e40p_pkg.sv"
    "$ROOT_DIR/rtl/core/include/cv32e40p_apu_core_pkg.sv"
    "$ROOT_DIR/rtl/core/include/cv32e40p_fpu_pkg.sv"
    "$ROOT_DIR/rtl/fpga/cv32e40p_clock_gate.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_top.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_sleep_unit.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_register_file_ff.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_prefetch_controller.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_prefetch_buffer.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_obi_interface.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_popcnt.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_mult.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_load_store_unit.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_int_controller.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_if_stage.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_id_stage.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_hwloop_regs.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_fifo.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_ff_one.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_ex_stage.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_cs_registers.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_decoder.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_core.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_controller.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_compressed_decoder.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_alu_div.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_apu_disp.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_alu.sv"
    "$ROOT_DIR/rtl/core/cv32e40p_aligner.sv"
    "$ROOT_DIR/rtl/soc/de2i150_cv32e40p_top.v"
    "$ROOT_DIR/sim/power_tb.sv"
)

usage() {
    cat <<'EOF'
Usage:
  sim/run_power_vcd.sh [options] [scenario ...]

Options:
  --list          Show available scenarios.
  --compile-only  Compile RTL/testbench only.
  --no-restore    Leave the last POWER_SIM firmware hex in firmware/.
  -h, --help      Show this help.

Default:
  Run all scenarios and restore the normal benchmark firmware at the end.

Environment overrides:
  VLIB, VLOG, VSIM, MAKE, TIMEOUT_CYCLES
EOF
}

list_scenarios() {
    for entry in "${SCENARIOS[@]}"; do
        IFS='|' read -r name _program _bench _variant <<< "$entry"
        printf '%s\n' "$name"
    done
}

compile_rtl() {
    mkdir -p "$BUILD_DIR" "$VCD_DIR"
    cd "$BUILD_DIR"
    ln -sfn "$ROOT_DIR/firmware" firmware

    if [[ ! -d work ]]; then
        "$VLIB_BIN" work
    fi

    "$VLOG_BIN" -sv -work work \
        "+incdir+$ROOT_DIR/rtl/core/include" \
        "${RTL_FILES[@]}"
}

build_power_firmware() {
    local program="$1"
    local bench_id="$2"
    local variant="$3"
    local defs

    defs="-DPOWER_SIM -DPOWER_SIM_VARIANT=$variant"
    if [[ "$program" == "benchmark" ]]; then
        defs="$defs -DPOWER_SIM_BENCH=$bench_id"
    fi
    defs="$defs -Wno-unused-function"

    "$MAKE_BIN" -C "$ROOT_DIR/firmware" \
        PROGRAM="$program" \
        EXTRA_CFLAGS="$defs" \
        clean all

    BUILT_POWER_FIRMWARE=1
}

restore_firmware() {
    "$MAKE_BIN" -C "$ROOT_DIR/firmware" \
        PROGRAM=benchmark \
        EXTRA_CFLAGS= \
        clean all
}

run_one() {
    local name="$1"
    local program="$2"
    local bench_id="$3"
    local variant="$4"
    local vcd_path="$VCD_DIR/${name}.vcd"

    printf '\n==> Building POWER_SIM firmware: %s\n' "$name"
    build_power_firmware "$program" "$bench_id" "$variant"

    printf '==> Running Questa simulation: %s\n' "$name"
    cd "$BUILD_DIR"
    "$VSIM_BIN" -c -voptargs=+acc work.tb_power \
        "+VCD=$vcd_path" \
        "+TIMEOUT_CYCLES=$TIMEOUT_CYCLES" \
        -do "run -all; quit -f"

    printf '==> Wrote %s\n' "$vcd_path"
}

find_scenario() {
    local wanted="$1"
    local entry

    for entry in "${SCENARIOS[@]}"; do
        IFS='|' read -r name program bench_id variant <<< "$entry"
        if [[ "$name" == "$wanted" ]]; then
            printf '%s|%s|%s|%s\n' "$name" "$program" "$bench_id" "$variant"
            return 0
        fi
    done

    return 1
}

cleanup() {
    if [[ "$RESTORE_FIRMWARE" -eq 1 && "$BUILT_POWER_FIRMWARE" -eq 1 ]]; then
        printf '\n==> Restoring normal benchmark firmware\n'
        restore_firmware
    fi
}
trap cleanup EXIT

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --list)
            list_scenarios
            exit 0
            ;;
        --compile-only)
            COMPILE_ONLY=1
            ;;
        --no-restore)
            RESTORE_FIRMWARE=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            REQUESTED+=("$1")
            ;;
    esac
    shift
done

if [[ "${#REQUESTED[@]}" -eq 0 ]]; then
    for entry in "${SCENARIOS[@]}"; do
        IFS='|' read -r name _program _bench _variant <<< "$entry"
        REQUESTED+=("$name")
    done
fi

compile_rtl

if [[ "$COMPILE_ONLY" -eq 1 ]]; then
    exit 0
fi

for wanted in "${REQUESTED[@]}"; do
    if ! entry="$(find_scenario "$wanted")"; then
        printf 'Unknown scenario: %s\n\nAvailable scenarios:\n' "$wanted" >&2
        list_scenarios >&2
        exit 2
    fi

    IFS='|' read -r name program bench_id variant <<< "$entry"
    run_one "$name" "$program" "$bench_id" "$variant"
done
