// SPDX-License-Identifier: Apache-2.0 WITH SHL-2.1
//
// FPGA-friendly replacement for cv32e40p_clock_gate.
//
// The upstream behavioural model in bhv/cv32e40p_sim_clock_gate.sv uses an
// always_latch + AND-gate of the clock. That structure is forbidden for both
// ASIC synthesis (it is a hazard-prone latch-based ICG) and FPGA synthesis
// (Cyclone IV / Quartus infer a transparent latch that can glitch the clock
// network and violate hold times). Intel's HDL style guide explicitly
// recommends using clock enables on flip-flops instead of gating the clock
// tree on FPGA.
//
// For functional bring-up on DE2i-150 we simply pass the free-running clock
// through. The sleep/WFI power optimisation is lost, but the core remains
// functionally correct because every sequential element inside the core is
// reset-initialised and the downstream logic only relies on clk_o toggling.
//
// When we eventually tape this out, replace this module with a technology
// primitive (e.g. ALTCLKCTRL for Intel, or a library ICG on ASIC).

module cv32e40p_clock_gate (
    input  logic clk_i,
    input  logic en_i,
    input  logic scan_cg_en_i,
    output logic clk_o
);

    // en_i / scan_cg_en_i are intentionally unused on FPGA:
    // we use clock-enable on FFs inside the core rather than gating the
    // clock net. See Intel 'Recommended HDL Coding Styles' (ug_qpp_hdl).
    // The (* keep *) attribute prevents Quartus from optimising the wire
    // away and producing a cryptic "no driver" warning.
    (* keep = 1 *) wire unused_ok = en_i | scan_cg_en_i;

    assign clk_o = clk_i;

endmodule
