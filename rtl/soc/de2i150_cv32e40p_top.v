// DE2i-150 + CV32E40P bring-up SoC
//
// Contents:
//   * cv32e40p_top (FPU=0, COREV_PULP=1, COREV_CLUSTER=0)
//   * 128 KB on-chip BRAM, dual-port, split into 4 byte lanes for M9K inference
//   * OBI <-> BRAM/MMIO shim (1-cycle RAM latency, UART can back-pressure)
//   * UART TX/RX MMIO at 0x0200_0000 / 0x0200_0004 / 0x0200_0008 (115200 8N1)
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
    localparam [31:0]  UART_RX_ADDR     = 32'h0200_0008;
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
    //   UART_STATUS: bit 0 TX-ready, bit 1 RX-valid, bit 2 RX-overrun,
    //                bit 3 RX-frame-error. Write bits 2/3 to clear errors.
    //   UART_TX: write byte 0 to transmit one character
    //   UART_RX: read byte 0 to pop one received character
    //   LED: data_addr == 0x0300_0000 (low byte is the visible status code)
    //   Anything else: always grant + rvalid so the LSU does not stall
    //                  forever if firmware scribbles on an unmapped addr
    // ------------------------------------------------------------------
    wire data_is_ram         = (data_addr < BRAM_SIZE_B);
    wire data_is_uart_status = (data_addr == UART_STATUS_ADDR);
    wire data_is_uart_tx     = (data_addr == UART_TX_ADDR);
    wire data_is_uart_rx     = (data_addr == UART_RX_ADDR);
    wire data_is_led         = (data_addr == LED_ADDR);

    // ------------------------------------------------------------------
    // UART TX/RX peripheral.
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

    wire [7:0] uart_rx_byte;
    wire       uart_rx_byte_valid;
    wire       uart_rx_frame_error_pulse;
    wire [7:0] uart_rx_data;
    wire       uart_rx_valid;
    wire       uart_rx_overrun;
    wire       uart_rx_frame_error;
    wire       uart_rx_read = data_req && data_gnt && data_is_uart_rx && !data_we;
    wire       uart_rx_pop  = uart_rx_read && uart_rx_valid;
    wire       uart_status_clear = data_req && data_gnt && data_is_uart_status &&
                                   data_we && data_be[0];

    uart_rx_8n1 #(
        .CLKS_PER_BIT(UART_CLKS_PER_BIT)
    ) u_uart_rx (
        .clk         (clk),
        .rst_n       (rst_n),
        .rx          (uart_rx),
        .rx_valid    (uart_rx_byte_valid),
        .rx_data     (uart_rx_byte),
        .frame_error (uart_rx_frame_error_pulse)
    );

    uart_rx_fifo #(
        .FIFO_AW(4)
    ) u_uart_rx_fifo (
        .clk               (clk),
        .rst_n             (rst_n),
        .push_valid        (uart_rx_byte_valid),
        .push_data         (uart_rx_byte),
        .pop               (uart_rx_pop),
        .frame_error_pulse (uart_rx_frame_error_pulse),
        .clear_overrun     (uart_status_clear && data_wdata[2]),
        .clear_frame_error (uart_status_clear && data_wdata[3]),
        .rdata             (uart_rx_data),
        .valid             (uart_rx_valid),
        .overrun           (uart_rx_overrun),
        .frame_error       (uart_rx_frame_error)
    );

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
    reg data_was_uart_rx_q;
    reg [31:0] data_rdata_uart_rx_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            instr_rvalid_q          <= 1'b0;
            data_rvalid_q           <= 1'b0;
            data_was_led_q          <= 1'b0;
            data_was_uart_status_q  <= 1'b0;
            data_was_uart_rx_q      <= 1'b0;
            data_rdata_uart_rx_q    <= 32'h0000_0000;
        end else begin
            instr_rvalid_q         <= instr_req & instr_gnt;
            data_rvalid_q          <= data_req  & data_gnt;
            data_was_led_q         <= data_req  & data_gnt & data_is_led;
            data_was_uart_status_q <= data_req  & data_gnt & data_is_uart_status;
            data_was_uart_rx_q     <= data_req  & data_gnt & data_is_uart_rx;
            if (uart_rx_read) begin
                data_rdata_uart_rx_q <= {24'h000000, uart_rx_data};
            end
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
                        data_was_uart_status_q ? {28'b0, uart_rx_frame_error,
                                                   uart_rx_overrun, uart_rx_valid,
                                                   uart_tx_ready} :
                        data_was_uart_rx_q ? data_rdata_uart_rx_q :
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

module uart_rx_8n1 #(
    parameter integer CLKS_PER_BIT = 434
) (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       rx,
    output reg        rx_valid,
    output reg  [7:0] rx_data,
    output reg        frame_error
);

    localparam [1:0] RX_IDLE  = 2'd0;
    localparam [1:0] RX_START = 2'd1;
    localparam [1:0] RX_DATA  = 2'd2;
    localparam [1:0] RX_STOP  = 2'd3;
    localparam integer HALF_CLKS_PER_BIT = CLKS_PER_BIT / 2;

    reg       rx_meta;
    reg       rx_sync;
    reg [1:0] state;
    reg [2:0] bit_index;
    reg [7:0] shift;
    reg [15:0] clk_count;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_meta     <= 1'b1;
            rx_sync     <= 1'b1;
            state       <= RX_IDLE;
            bit_index   <= 3'd0;
            shift       <= 8'h00;
            clk_count   <= 16'd0;
            rx_valid    <= 1'b0;
            rx_data     <= 8'h00;
            frame_error <= 1'b0;
        end else begin
            rx_meta     <= rx;
            rx_sync     <= rx_meta;
            rx_valid    <= 1'b0;
            frame_error <= 1'b0;

            case (state)
                RX_IDLE: begin
                    clk_count <= 16'd0;
                    bit_index <= 3'd0;
                    if (rx_sync == 1'b0) begin
                        state <= RX_START;
                    end
                end

                RX_START: begin
                    if (clk_count == (HALF_CLKS_PER_BIT - 1)) begin
                        clk_count <= 16'd0;
                        if (rx_sync == 1'b0) begin
                            state <= RX_DATA;
                        end else begin
                            state <= RX_IDLE;
                        end
                    end else begin
                        clk_count <= clk_count + 16'd1;
                    end
                end

                RX_DATA: begin
                    if (clk_count == (CLKS_PER_BIT - 1)) begin
                        clk_count        <= 16'd0;
                        shift[bit_index] <= rx_sync;
                        if (bit_index == 3'd7) begin
                            state <= RX_STOP;
                        end else begin
                            bit_index <= bit_index + 3'd1;
                        end
                    end else begin
                        clk_count <= clk_count + 16'd1;
                    end
                end

                RX_STOP: begin
                    if (clk_count == (CLKS_PER_BIT - 1)) begin
                        clk_count <= 16'd0;
                        if (rx_sync == 1'b1) begin
                            rx_data  <= shift;
                            rx_valid <= 1'b1;
                        end else begin
                            frame_error <= 1'b1;
                        end
                        state <= RX_IDLE;
                    end else begin
                        clk_count <= clk_count + 16'd1;
                    end
                end

                default: begin
                    state <= RX_IDLE;
                end
            endcase
        end
    end

endmodule

module uart_rx_fifo #(
    parameter integer FIFO_AW = 4
) (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       push_valid,
    input  wire [7:0] push_data,
    input  wire       pop,
    input  wire       frame_error_pulse,
    input  wire       clear_overrun,
    input  wire       clear_frame_error,
    output wire [7:0] rdata,
    output wire       valid,
    output reg        overrun,
    output reg        frame_error
);

    localparam integer FIFO_DEPTH = (1 << FIFO_AW);
    localparam [FIFO_AW:0] FIFO_DEPTH_COUNT = (1 << FIFO_AW);

    (* ramstyle = "logic" *) reg [7:0] fifo [0:FIFO_DEPTH-1];
    reg [FIFO_AW-1:0] rd_ptr;
    reg [FIFO_AW-1:0] wr_ptr;
    reg [FIFO_AW:0] count;

    wire fifo_full = (count == FIFO_DEPTH_COUNT);
    wire do_pop = pop && valid;
    wire do_push = push_valid && (!fifo_full || do_pop);
    wire push_overrun = push_valid && fifo_full && !do_pop;

    assign valid = (count != 0);
    assign rdata = fifo[rd_ptr];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_ptr      <= {FIFO_AW{1'b0}};
            wr_ptr      <= {FIFO_AW{1'b0}};
            count       <= 0;
            overrun     <= 1'b0;
            frame_error <= 1'b0;
        end else begin
            if (clear_overrun) begin
                overrun <= 1'b0;
            end
            if (clear_frame_error) begin
                frame_error <= 1'b0;
            end
            if (push_overrun) begin
                overrun <= 1'b1;
            end
            if (frame_error_pulse) begin
                frame_error <= 1'b1;
            end

            if (do_push) begin
                fifo[wr_ptr] <= push_data;
                wr_ptr <= wr_ptr + 1'b1;
            end
            if (do_pop) begin
                rd_ptr <= rd_ptr + 1'b1;
            end

            case ({do_push, do_pop})
                2'b10: count <= count + 1'b1;
                2'b01: count <= count - 1'b1;
                default: count <= count;
            endcase
        end
    end

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
