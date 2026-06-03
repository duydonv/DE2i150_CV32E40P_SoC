// DE2i-150 + CV32E40P bring-up SoC
//
// Contents:
//   * cv32e40p_top (FPU=0, COREV_PULP=1, COREV_CLUSTER=0)
//   * 128 KB on-chip BRAM, dual-port, split into 4 byte lanes for M9K inference
//   * OBI <-> BRAM/MMIO shim (1-cycle RAM latency, UART can back-pressure)
//   * UART TX-only MMIO at 0x0200_0000 / 0x0200_0004 (115200 8N1)
//   * 18-bit MMIO LED register at 0x0300_0000
//   * reset synchroniser on KEY0, hardware heartbeat on LEDG0
//
// This file is plain Verilog-2001 on purpose so we do not rely on the
// SystemVerilog features that Quartus Standard Lite parses inconsistently.
// The CV32E40P core files are still SystemVerilog; they are pulled in via
// the SYSTEMVERILOG_FILE assignment in the QSF (see README).

`timescale 1ns/1ps

module de2i150_cv32e40p_top (
    input  wire        clock_50,
    input  wire        reset_n,      // KEY0, active-low
    output wire        uart_tx,
    input  wire        uart_rx,
    output wire [17:0] ledr,
    output wire [ 1:0] ledg
);

    // ------------------------------------------------------------------
    // Parameters
    // ------------------------------------------------------------------
    localparam integer BRAM_AW_WORDS = 15;                   // 32768 words = 128 KB
    localparam integer BRAM_SIZE_B   = (1 << BRAM_AW_WORDS) * 4;
    localparam integer UART_CLKS_PER_BIT = 434;              // 50 MHz / 115200 baud
    localparam [31:0]  UART_STATUS_ADDR = 32'h0200_0000;
    localparam [31:0]  UART_TX_ADDR     = 32'h0200_0004;
    localparam [31:0]  LED_ADDR      = 32'h0300_0000;
    localparam [31:0]  BOOT_ADDR     = 32'h0000_0000;        // matches _start in sections.lds
    localparam [31:0]  MTVEC_ADDR    = 32'h0000_0040;        // exception vectors start 64 B in
    localparam [31:0]  DM_HALT_ADDR  = 32'h1A11_0800;
    localparam [31:0]  DM_EXCP_ADDR  = 32'h1A14_0000;

    wire clk = clock_50;

    // ------------------------------------------------------------------
    // Reset synchroniser
    // Async-assert, sync-deassert. The core expects active-low rst_ni.
    // ------------------------------------------------------------------
    reg [3:0] rstn_sync;
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) rstn_sync <= 4'h0;
        else          rstn_sync <= {rstn_sync[2:0], 1'b1};
    end
    wire rst_n = rstn_sync[3];

    // ------------------------------------------------------------------
    // Core <-> SoC wires (OBI-style)
    // ------------------------------------------------------------------
    wire        instr_req;
    wire        instr_gnt;
    wire        instr_rvalid;
    wire [31:0] instr_addr;
    wire [31:0] instr_rdata;

    wire        data_req;
    wire        data_gnt;
    wire        data_rvalid;
    wire        data_we;
    wire [ 3:0] data_be;
    wire [31:0] data_addr;
    wire [31:0] data_wdata;
    wire [31:0] data_rdata;

    wire        core_sleep;
    wire        irq_ack;
    wire [ 4:0] irq_id;

    // ------------------------------------------------------------------
    // CV32E40P instantiation
    // FPU=0 so cv32e40p_clock_gate is only used inside sleep_unit; we
    // substitute our FPGA pass-through shim via the search path.
    // ------------------------------------------------------------------
    cv32e40p_top #(
        .COREV_PULP      (1),
        .COREV_CLUSTER   (0),
        .FPU             (0),
        .FPU_ADDMUL_LAT  (0),
        .FPU_OTHERS_LAT  (0),
        .ZFINX           (0),
        .NUM_MHPMCOUNTERS(1),
        .FPGA_TIMING_MODE(1)
    ) u_core (
        .clk_i              (clk),
        .rst_ni             (rst_n),

        .pulp_clock_en_i    (1'b1),
        .scan_cg_en_i       (1'b0),

        .boot_addr_i        (BOOT_ADDR),
        .mtvec_addr_i       (MTVEC_ADDR),
        .dm_halt_addr_i     (DM_HALT_ADDR),
        .hart_id_i          (32'h0000_0000),
        .dm_exception_addr_i(DM_EXCP_ADDR),

        .instr_req_o        (instr_req),
        .instr_gnt_i        (instr_gnt),
        .instr_rvalid_i     (instr_rvalid),
        .instr_addr_o       (instr_addr),
        .instr_rdata_i      (instr_rdata),

        .data_req_o         (data_req),
        .data_gnt_i         (data_gnt),
        .data_rvalid_i      (data_rvalid),
        .data_we_o          (data_we),
        .data_be_o          (data_be),
        .data_addr_o        (data_addr),
        .data_wdata_o       (data_wdata),
        .data_rdata_i       (data_rdata),

        .irq_i              (32'h0000_0000),
        .irq_ack_o          (irq_ack),
        .irq_id_o           (irq_id),

        .debug_req_i        (1'b0),
        .debug_havereset_o  (),
        .debug_running_o    (),
        .debug_halted_o     (),

        .fetch_enable_i     (1'b1),
        .core_sleep_o       (core_sleep)
    );

    // ------------------------------------------------------------------
    // Address decode (data port)
    //   RAM: data_addr < BRAM_SIZE_B
    //   UART_STATUS: bit 0 is TX-ready
    //   UART_TX: write byte 0 to transmit one character
    //   LED: data_addr == 0x0300_0000 (low byte is the visible status code)
    //   Anything else: always grant + rvalid so the LSU does not stall
    //                  forever if firmware scribbles on an unmapped addr
    // ------------------------------------------------------------------
    wire data_is_ram         = (data_addr < BRAM_SIZE_B);
    wire data_is_uart_status = (data_addr == UART_STATUS_ADDR);
    wire data_is_uart_tx     = (data_addr == UART_TX_ADDR);
    wire data_is_led         = (data_addr == LED_ADDR);

    // ------------------------------------------------------------------
    // UART TX-only peripheral.
    // ------------------------------------------------------------------
    wire uart_tx_ready;
    wire uart_tx_write = data_req && data_is_uart_tx && data_we && data_be[0];
    wire uart_tx_valid = uart_tx_write && data_gnt;

    uart_tx_8n1 #(
        .CLKS_PER_BIT(UART_CLKS_PER_BIT)
    ) u_uart_tx (
        .clk       (clk),
        .rst_n     (rst_n),
        .tx_valid  (uart_tx_valid),
        .tx_data   (data_wdata[7:0]),
        .tx_ready  (uart_tx_ready),
        .tx        (uart_tx)
    );

    // RX is intentionally unused for the first benchmark-output path.
    wire unused_uart_rx = uart_rx;

    // ------------------------------------------------------------------
    // OBI handshake
    //   RAM/LED/status: accept in 1 cycle
    //   UART TX write : hold data_gnt low while the shifter is busy
    //   rvalid     (data returned 1 cycle later, matches BRAM latency)
    // ------------------------------------------------------------------
    assign instr_gnt = instr_req;
    assign data_gnt  = data_req && (!uart_tx_write || uart_tx_ready);

    reg instr_rvalid_q;
    reg data_rvalid_q;
    reg data_was_led_q;
    reg data_was_uart_status_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            instr_rvalid_q          <= 1'b0;
            data_rvalid_q           <= 1'b0;
            data_was_led_q          <= 1'b0;
            data_was_uart_status_q  <= 1'b0;
        end else begin
            instr_rvalid_q         <= instr_req & instr_gnt;
            data_rvalid_q          <= data_req  & data_gnt;
            data_was_led_q         <= data_req  & data_gnt & data_is_led;
            data_was_uart_status_q <= data_req  & data_gnt & data_is_uart_status;
        end
    end
    assign instr_rvalid = instr_rvalid_q;
    assign data_rvalid  = data_rvalid_q;

    // ------------------------------------------------------------------
    // 128 KB on-chip BRAM, true dual-port, 4 byte lanes (M9K-friendly)
    //   Port A (read only) : instruction fetch
    //   Port B (R/W)       : data load/store
    // ------------------------------------------------------------------
    wire [BRAM_AW_WORDS-1:0] a_word_addr = instr_addr[BRAM_AW_WORDS+1:2];
    wire [BRAM_AW_WORDS-1:0] b_word_addr = data_addr [BRAM_AW_WORDS+1:2];

    (* ramstyle = "M9K" *) reg [7:0] mem0 [0:(1<<BRAM_AW_WORDS)-1];
    (* ramstyle = "M9K" *) reg [7:0] mem1 [0:(1<<BRAM_AW_WORDS)-1];
    (* ramstyle = "M9K" *) reg [7:0] mem2 [0:(1<<BRAM_AW_WORDS)-1];
    (* ramstyle = "M9K" *) reg [7:0] mem3 [0:(1<<BRAM_AW_WORDS)-1];

    initial begin
        $readmemh("firmware/firmware_byte0.hex", mem0);
        $readmemh("firmware/firmware_byte1.hex", mem1);
        $readmemh("firmware/firmware_byte2.hex", mem2);
        $readmemh("firmware/firmware_byte3.hex", mem3);
    end

    reg [31:0] instr_rdata_q;
    reg [31:0] data_rdata_ram_q;

    always @(posedge clk) begin
        if (instr_req) begin
            instr_rdata_q <= {mem3[a_word_addr],
                              mem2[a_word_addr],
                              mem1[a_word_addr],
                              mem0[a_word_addr]};
        end

        if (data_req && data_gnt && data_is_ram) begin
            if (data_we) begin
                if (data_be[0]) mem0[b_word_addr] <= data_wdata[ 7: 0];
                if (data_be[1]) mem1[b_word_addr] <= data_wdata[15: 8];
                if (data_be[2]) mem2[b_word_addr] <= data_wdata[23:16];
                if (data_be[3]) mem3[b_word_addr] <= data_wdata[31:24];
            end
            data_rdata_ram_q <= {mem3[b_word_addr],
                                 mem2[b_word_addr],
                                 mem1[b_word_addr],
                                 mem0[b_word_addr]};
        end
    end

    assign instr_rdata = instr_rdata_q;

    // ------------------------------------------------------------------
    // LED MMIO (lower 18 bits of 0x0300_0000)
    // ------------------------------------------------------------------
    reg [17:0] led_reg;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            led_reg <= 18'h00000;
        end else if (data_req && data_gnt && data_is_led && data_we) begin
            if (data_be[0]) led_reg[ 7: 0] <= data_wdata[ 7: 0];
            if (data_be[1]) led_reg[15: 8] <= data_wdata[15: 8];
            if (data_be[2]) led_reg[17:16] <= data_wdata[17:16];
        end
    end

    assign data_rdata = data_was_led_q ? {14'b0, led_reg} :
                        data_was_uart_status_q ? {31'b0, uart_tx_ready} :
                        data_rdata_ram_q;

    // ------------------------------------------------------------------
    // Hardware heartbeat on LEDG0 so we can tell at a glance the clock
    // network is alive even if the CPU is stuck. LEDG1 lights if the
    // core enters sleep state (useful debug hint).
    // ------------------------------------------------------------------
    reg [25:0] hb_div;
    always @(posedge clk) hb_div <= hb_div + 26'd1;

    assign ledr = {10'b0, led_reg[7:0]};
    assign ledg = {core_sleep, hb_div[25]};

endmodule

module uart_tx_8n1 #(
    parameter integer CLKS_PER_BIT = 434
) (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       tx_valid,
    input  wire [7:0] tx_data,
    output wire       tx_ready,
    output wire       tx
);

    reg [9:0]  shifter;
    reg [3:0]  bit_count;
    reg [15:0] clk_count;

    assign tx_ready = (bit_count == 4'd0);
    assign tx       = tx_ready ? 1'b1 : shifter[0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            shifter   <= 10'h3ff;
            bit_count <= 4'd0;
            clk_count <= 16'd0;
        end else if (tx_ready) begin
            clk_count <= 16'd0;
            if (tx_valid) begin
                shifter   <= {1'b1, tx_data, 1'b0};
                bit_count <= 4'd10;
            end
        end else if (clk_count == (CLKS_PER_BIT - 1)) begin
            clk_count <= 16'd0;
            shifter   <= {1'b1, shifter[9:1]};
            bit_count <= bit_count - 4'd1;
        end else begin
            clk_count <= clk_count + 16'd1;
        end
    end

endmodule
