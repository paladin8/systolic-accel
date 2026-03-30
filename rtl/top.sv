// Top-level integration: scratchpads + controller + systolic array.
// Double-buffered SP_A/SP_B/SP_C: two banks each, selected by bank_sel.
// External port provides host access to scratchpads; host can load the
// non-active bank while the controller computes from the active bank.
module top #(
    parameter ROWS           = 4,
    parameter COLS           = 4,
    parameter DATA_WIDTH     = 16,
    parameter ACC_WIDTH      = 32,
    parameter PIPELINE_DEPTH = 2,
    parameter SP_A_DEPTH     = 64,
    parameter SP_B_DEPTH     = 64,
    parameter SP_C_DEPTH     = 64,
    parameter EXT_ADDR_W     = $clog2(SP_A_DEPTH)  // max(A,B,C) depths assumed equal
)(
    input  logic        clk,
    input  logic        rst_n,

    // Computation control
    input  logic        start,
    output logic        done,
    input  logic [15:0] dim_m,
    input  logic [15:0] dim_k,
    input  logic [15:0] dim_n,

    // Bank selection
    input  logic        bank_sel,   // which A/B bank the controller reads from
    input  logic        ext_bank,   // which A/B bank the external host accesses

    // External scratchpad access
    input  logic [1:0]                    ext_sel,   // 0=A, 1=B, 2=C
    input  logic [EXT_ADDR_W-1:0]         ext_addr,
    input  logic                          ext_we,
    input  logic [COLS*ACC_WIDTH-1:0]     ext_wdata,
    output logic [COLS*ACC_WIDTH-1:0]     ext_rdata
);

    // ── Width localparams ───────────────────────────────────────────────
    localparam A_WIDTH = ROWS * DATA_WIDTH;
    localparam B_WIDTH = COLS * DATA_WIDTH;
    localparam C_WIDTH = COLS * ACC_WIDTH;
    // EXT_ADDR_W is now a parameter (Yosys 0.33 compatibility)

    // ── Scratchpad port signals (per bank) ────────────────────────────
    // SP_A bank 0
    logic                            sp_a0_we;
    logic [$clog2(SP_A_DEPTH)-1:0]   sp_a0_addr;
    logic [A_WIDTH-1:0]              sp_a0_wdata;
    logic [A_WIDTH-1:0]              sp_a0_rdata;

    // SP_A bank 1
    logic                            sp_a1_we;
    logic [$clog2(SP_A_DEPTH)-1:0]   sp_a1_addr;
    logic [A_WIDTH-1:0]              sp_a1_wdata;
    logic [A_WIDTH-1:0]              sp_a1_rdata;

    // SP_B bank 0
    logic                            sp_b0_we;
    logic [$clog2(SP_B_DEPTH)-1:0]   sp_b0_addr;
    logic [B_WIDTH-1:0]              sp_b0_wdata;
    logic [B_WIDTH-1:0]              sp_b0_rdata;

    // SP_B bank 1
    logic                            sp_b1_we;
    logic [$clog2(SP_B_DEPTH)-1:0]   sp_b1_addr;
    logic [B_WIDTH-1:0]              sp_b1_wdata;
    logic [B_WIDTH-1:0]              sp_b1_rdata;

    // SP_C bank 0
    logic                            sp_c0_we;
    logic [$clog2(SP_C_DEPTH)-1:0]   sp_c0_addr;
    logic [C_WIDTH-1:0]              sp_c0_wdata;
    logic [C_WIDTH-1:0]              sp_c0_rdata;

    // SP_C bank 1
    logic                            sp_c1_we;
    logic [$clog2(SP_C_DEPTH)-1:0]   sp_c1_addr;
    logic [C_WIDTH-1:0]              sp_c1_wdata;
    logic [C_WIDTH-1:0]              sp_c1_rdata;

    // ── Controller ↔ scratchpad signals ─────────────────────────────────
    logic [$clog2(SP_A_DEPTH)-1:0]   ctrl_a_addr;
    logic [$clog2(SP_B_DEPTH)-1:0]   ctrl_b_addr;
    logic [$clog2(SP_C_DEPTH)-1:0]   ctrl_c_addr;
    logic                            ctrl_c_we;
    logic [C_WIDTH-1:0]              ctrl_c_wdata;

    // Controller rdata — muxed from active bank
    logic [A_WIDTH-1:0]              ctrl_a_rdata;
    logic [B_WIDTH-1:0]              ctrl_b_rdata;
    logic [C_WIDTH-1:0]              ctrl_c_rdata;

    // ── Array signals ───────────────────────────────────────────────────
    logic                            arr_enable;
    logic                            arr_load_weight;
    logic [A_WIDTH-1:0]              arr_a_in;
    logic [B_WIDTH-1:0]              arr_b_in;
    logic [C_WIDTH-1:0]              arr_drain_out;

    // ── Track whether controller owns scratchpad ports ─────────────────
    // Controller needs the ports starting from the cycle `start` is asserted
    // (for the pre-read in IDLE), through done. Use a combinational signal
    // that includes `start` so the mux switches immediately.
    // Synthesizes to: 1 flip-flop
    logic ctrl_running_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            ctrl_running_reg <= 1'b0;
        else if (start && !ctrl_running_reg)
            ctrl_running_reg <= 1'b1;
        else if (done)
            ctrl_running_reg <= 1'b0;
    end

    logic ctrl_running;
    assign ctrl_running = ctrl_running_reg || start;

    // ── Active bank latching ──────────────────────────────────────────
    // Sampled on start, held during computation. Combinational signal
    // ensures correct bank is selected on the same cycle start is asserted
    // (controller pre-reads SP_B in S_IDLE when start is high).
    // Synthesizes to: 1 flip-flop
    logic active_bank_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            active_bank_reg <= 1'b0;
        else if (start && !ctrl_running_reg)
            active_bank_reg <= bank_sel;
    end

    logic active_bank;
    assign active_bank = (start && !ctrl_running_reg) ? bank_sel : active_bank_reg;

    // ── SP_A bank port muxing ─────────────────────────────────────────
    // Each bank instance has one port, driven by exactly one source:
    //   - Controller (during computation, active bank)
    //   - Host (during computation non-active bank, or idle selected bank)
    //   - Default (addr=0, we=0, wdata=0)

    // SP_A bank 0
    always_comb begin
        if (ctrl_running && active_bank == 1'b0) begin
            // Controller owns bank 0
            sp_a0_addr  = ctrl_a_addr;
            sp_a0_we    = 1'b0;
            sp_a0_wdata = '0;
        end else if (ext_bank == 1'b0 && (!ctrl_running || active_bank != 1'b0)) begin
            // Host accesses bank 0
            sp_a0_addr  = ext_addr[$clog2(SP_A_DEPTH)-1:0];
            sp_a0_we    = ext_we && (ext_sel == 2'd0);
            sp_a0_wdata = ext_wdata[A_WIDTH-1:0];
        end else begin
            // Nobody needs bank 0
            sp_a0_addr  = '0;
            sp_a0_we    = 1'b0;
            sp_a0_wdata = '0;
        end
    end

    // SP_A bank 1
    always_comb begin
        if (ctrl_running && active_bank == 1'b1) begin
            // Controller owns bank 1
            sp_a1_addr  = ctrl_a_addr;
            sp_a1_we    = 1'b0;
            sp_a1_wdata = '0;
        end else if (ext_bank == 1'b1 && (!ctrl_running || active_bank != 1'b1)) begin
            // Host accesses bank 1
            sp_a1_addr  = ext_addr[$clog2(SP_A_DEPTH)-1:0];
            sp_a1_we    = ext_we && (ext_sel == 2'd0);
            sp_a1_wdata = ext_wdata[A_WIDTH-1:0];
        end else begin
            // Nobody needs bank 1
            sp_a1_addr  = '0;
            sp_a1_we    = 1'b0;
            sp_a1_wdata = '0;
        end
    end

    // Controller reads from active A bank
    assign ctrl_a_rdata = active_bank ? sp_a1_rdata : sp_a0_rdata;

    // ── SP_B bank port muxing ─────────────────────────────────────────

    // SP_B bank 0
    always_comb begin
        if (ctrl_running && active_bank == 1'b0) begin
            sp_b0_addr  = ctrl_b_addr;
            sp_b0_we    = 1'b0;
            sp_b0_wdata = '0;
        end else if (ext_bank == 1'b0 && (!ctrl_running || active_bank != 1'b0)) begin
            sp_b0_addr  = ext_addr[$clog2(SP_B_DEPTH)-1:0];
            sp_b0_we    = ext_we && (ext_sel == 2'd1);
            sp_b0_wdata = ext_wdata[B_WIDTH-1:0];
        end else begin
            sp_b0_addr  = '0;
            sp_b0_we    = 1'b0;
            sp_b0_wdata = '0;
        end
    end

    // SP_B bank 1
    always_comb begin
        if (ctrl_running && active_bank == 1'b1) begin
            sp_b1_addr  = ctrl_b_addr;
            sp_b1_we    = 1'b0;
            sp_b1_wdata = '0;
        end else if (ext_bank == 1'b1 && (!ctrl_running || active_bank != 1'b1)) begin
            sp_b1_addr  = ext_addr[$clog2(SP_B_DEPTH)-1:0];
            sp_b1_we    = ext_we && (ext_sel == 2'd1);
            sp_b1_wdata = ext_wdata[B_WIDTH-1:0];
        end else begin
            sp_b1_addr  = '0;
            sp_b1_we    = 1'b0;
            sp_b1_wdata = '0;
        end
    end

    // Controller reads from active B bank
    assign ctrl_b_rdata = active_bank ? sp_b1_rdata : sp_b0_rdata;

    // ── SP_C bank port muxing ─────────────────────────────────────────
    // Same pattern as A/B, but controller reads AND writes the active C bank.

    // SP_C bank 0
    always_comb begin
        if (ctrl_running && active_bank == 1'b0) begin
            sp_c0_addr  = ctrl_c_addr;
            sp_c0_we    = ctrl_c_we;
            sp_c0_wdata = ctrl_c_wdata;
        end else if (ext_bank == 1'b0 && (!ctrl_running || active_bank != 1'b0)) begin
            sp_c0_addr  = ext_addr[$clog2(SP_C_DEPTH)-1:0];
            sp_c0_we    = ext_we && (ext_sel == 2'd2);
            sp_c0_wdata = ext_wdata[C_WIDTH-1:0];
        end else begin
            sp_c0_addr  = '0;
            sp_c0_we    = 1'b0;
            sp_c0_wdata = '0;
        end
    end

    // SP_C bank 1
    always_comb begin
        if (ctrl_running && active_bank == 1'b1) begin
            sp_c1_addr  = ctrl_c_addr;
            sp_c1_we    = ctrl_c_we;
            sp_c1_wdata = ctrl_c_wdata;
        end else if (ext_bank == 1'b1 && (!ctrl_running || active_bank != 1'b1)) begin
            sp_c1_addr  = ext_addr[$clog2(SP_C_DEPTH)-1:0];
            sp_c1_we    = ext_we && (ext_sel == 2'd2);
            sp_c1_wdata = ext_wdata[C_WIDTH-1:0];
        end else begin
            sp_c1_addr  = '0;
            sp_c1_we    = 1'b0;
            sp_c1_wdata = '0;
        end
    end

    // Controller reads from active C bank
    assign ctrl_c_rdata = active_bank ? sp_c1_rdata : sp_c0_rdata;

    // ── External read data mux ────────────────────────────────────────
    // Host reads from the bank selected by ext_bank
    logic [A_WIDTH-1:0] ext_a_rdata;
    logic [B_WIDTH-1:0] ext_b_rdata;
    logic [C_WIDTH-1:0] ext_c_rdata;

    assign ext_a_rdata = ext_bank ? sp_a1_rdata : sp_a0_rdata;
    assign ext_b_rdata = ext_bank ? sp_b1_rdata : sp_b0_rdata;
    assign ext_c_rdata = ext_bank ? sp_c1_rdata : sp_c0_rdata;

    always_comb begin
        case (ext_sel)
            2'd0:    ext_rdata = {{(C_WIDTH-A_WIDTH){1'b0}}, ext_a_rdata};
            2'd1:    ext_rdata = {{(C_WIDTH-B_WIDTH){1'b0}}, ext_b_rdata};
            2'd2:    ext_rdata = ext_c_rdata;
            default: ext_rdata = '0;
        endcase
    end

    // ── Scratchpad instances ────────────────────────────────────────────

    scratchpad #(
        .DEPTH(SP_A_DEPTH),
        .WIDTH(A_WIDTH)
    ) sp_a0 (
        .clk(clk),
        .we(sp_a0_we),
        .addr(sp_a0_addr),
        .wdata(sp_a0_wdata),
        .rdata(sp_a0_rdata)
    );

    scratchpad #(
        .DEPTH(SP_A_DEPTH),
        .WIDTH(A_WIDTH)
    ) sp_a1 (
        .clk(clk),
        .we(sp_a1_we),
        .addr(sp_a1_addr),
        .wdata(sp_a1_wdata),
        .rdata(sp_a1_rdata)
    );

    scratchpad #(
        .DEPTH(SP_B_DEPTH),
        .WIDTH(B_WIDTH)
    ) sp_b0 (
        .clk(clk),
        .we(sp_b0_we),
        .addr(sp_b0_addr),
        .wdata(sp_b0_wdata),
        .rdata(sp_b0_rdata)
    );

    scratchpad #(
        .DEPTH(SP_B_DEPTH),
        .WIDTH(B_WIDTH)
    ) sp_b1 (
        .clk(clk),
        .we(sp_b1_we),
        .addr(sp_b1_addr),
        .wdata(sp_b1_wdata),
        .rdata(sp_b1_rdata)
    );

    scratchpad #(
        .DEPTH(SP_C_DEPTH),
        .WIDTH(C_WIDTH)
    ) sp_c0 (
        .clk(clk),
        .we(sp_c0_we),
        .addr(sp_c0_addr),
        .wdata(sp_c0_wdata),
        .rdata(sp_c0_rdata)
    );

    scratchpad #(
        .DEPTH(SP_C_DEPTH),
        .WIDTH(C_WIDTH)
    ) sp_c1 (
        .clk(clk),
        .we(sp_c1_we),
        .addr(sp_c1_addr),
        .wdata(sp_c1_wdata),
        .rdata(sp_c1_rdata)
    );

    // ── Systolic array instance ─────────────────────────────────────────

    systolic_array #(
        .ROWS(ROWS),
        .COLS(COLS),
        .DATA_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .PIPELINE_DEPTH(PIPELINE_DEPTH)
    ) array_inst (
        .clk(clk),
        .rst_n(rst_n),
        .enable(arr_enable),
        .load_weight(arr_load_weight),
        .a_in(arr_a_in),
        .b_in(arr_b_in),
        .drain_out(arr_drain_out)
    );

    // ── Controller instance ─────────────────────────────────────────────

    controller #(
        .ROWS(ROWS),
        .COLS(COLS),
        .DATA_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .PIPELINE_DEPTH(PIPELINE_DEPTH),
        .SP_A_DEPTH(SP_A_DEPTH),
        .SP_B_DEPTH(SP_B_DEPTH),
        .SP_C_DEPTH(SP_C_DEPTH)
    ) ctrl (
        .clk(clk),
        .rst_n(rst_n),
        .start(start),
        .done(done),
        .dim_m(dim_m),
        .dim_k(dim_k),
        .dim_n(dim_n),
        .sp_a_addr(ctrl_a_addr),
        .sp_a_rdata(ctrl_a_rdata),
        .sp_b_addr(ctrl_b_addr),
        .sp_b_rdata(ctrl_b_rdata),
        .sp_c_addr(ctrl_c_addr),
        .sp_c_we(ctrl_c_we),
        .sp_c_wdata(ctrl_c_wdata),
        .sp_c_rdata(ctrl_c_rdata),
        .arr_enable(arr_enable),
        .arr_load_weight(arr_load_weight),
        .arr_a_in(arr_a_in),
        .arr_b_in(arr_b_in),
        .arr_drain_out(arr_drain_out)
    );

endmodule
