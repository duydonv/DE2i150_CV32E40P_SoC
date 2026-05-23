`timescale 1ns/1ps

module tb_power;
    localparam [7:0] LED_POWER_START = 8'h40;
    localparam [7:0] LED_PASS        = 8'ha5;
    localparam [7:0] LED_FAIL        = 8'hef;

    reg         clock_50 = 1'b0;
    reg         reset_n  = 1'b0;
    reg         uart_rx  = 1'b1;
    wire        uart_tx;
    wire [17:0] ledr;
    wire [ 1:0] ledg;

    string vcd_path;
    int unsigned timeout_cycles;
    bit dump_started;

    de2i150_cv32e40p_top u_dut (
        .clock_50(clock_50),
        .reset_n (reset_n),
        .uart_tx (uart_tx),
        .uart_rx (uart_rx),
        .ledr    (ledr),
        .ledg    (ledg)
    );

    always #10 clock_50 = ~clock_50; // 50 MHz

    initial begin
        if (!$value$plusargs("VCD=%s", vcd_path)) begin
            vcd_path = "power.vcd";
        end

        if (!$value$plusargs("TIMEOUT_CYCLES=%d", timeout_cycles)) begin
            timeout_cycles = 2_000_000;
        end

        repeat (10) @(posedge clock_50);
        reset_n <= 1'b1;
    end

    initial begin
        dump_started = 1'b0;

        wait (reset_n === 1'b1);
        wait (ledr[7:0] === LED_POWER_START);

        $display("[%0t] Power activity window started: %s", $time, vcd_path);
        $dumpfile(vcd_path);
        $dumpvars(0, u_dut);
        dump_started = 1'b1;

        wait ((ledr[7:0] === LED_PASS) || (ledr[7:0] === LED_FAIL));
        repeat (20) @(posedge clock_50);
        $dumpoff;

        if (ledr[7:0] === LED_PASS) begin
            $display("[%0t] Power simulation completed: PASS", $time);
            $finish;
        end else begin
            $fatal(1, "[%0t] Power simulation completed: checksum FAIL", $time);
        end
    end

    initial begin
        repeat (timeout_cycles) @(posedge clock_50);
        if (dump_started) begin
            $dumpoff;
        end
        $fatal(1, "[%0t] Power simulation timeout after %0d cycles",
            $time, timeout_cycles);
    end
endmodule
