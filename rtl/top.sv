// Top-level integration: scratchpads + controller + systolic array.
// External port provides host access to scratchpads when controller is IDLE.
module top #(
    parameter ROWS           = 4,
    parameter COLS           = 4,
    parameter DATA_WIDTH     = 16,
    parameter ACC_WIDTH      = 32,
    parameter PIPELINE_DEPTH = 2,
    parameter SP_A_DEPTH     = 64,
    parameter SP_B_DEPTH     = 64,
    parameter SP_C_DEPTH     = 64
)(
    input  logic        clk,
    input  logic        rst_n,

    // Computation control
    input  logic        start,
    output logic        done,
    input  logic [15:0] dim_m,
    input  logic [15:0] dim_k,
    input  logic [15:0] dim_n,

    // External scratchpad access (active when controller is IDLE)
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
    localparam MAX_SP_DEPTH = (SP_A_DEPTH > SP_B_DEPTH)
                            ? (SP_A_DEPTH > SP_C_DEPTH ? SP_A_DEPTH : SP_C_DEPTH)
                            : (SP_B_DEPTH > SP_C_DEPTH ? SP_B_DEPTH : SP_C_DEPTH);
    localparam EXT_ADDR_W = $clog2(MAX_SP_DEPTH);

    // ── Scratchpad port signals ─────────────────────────────────────────
    // SP_A
    logic                            sp_a_we;
    logic [$clog2(SP_A_DEPTH)-1:0]   sp_a_addr;
    logic [A_WIDTH-1:0]              sp_a_wdata;
    logic [A_WIDTH-1:0]              sp_a_rdata;

    // SP_B
    logic                            sp_b_we;
    logic [$clog2(SP_B_DEPTH)-1:0]   sp_b_addr;
    logic [B_WIDTH-1:0]              sp_b_wdata;
    logic [B_WIDTH-1:0]              sp_b_rdata;

    // SP_C
    logic                            sp_c_we;
    logic [$clog2(SP_C_DEPTH)-1:0]   sp_c_addr;
    logic [C_WIDTH-1:0]              sp_c_wdata;
    logic [C_WIDTH-1:0]              sp_c_rdata;

    // ── Controller ↔ scratchpad signals ─────────────────────────────────
    logic [$clog2(SP_A_DEPTH)-1:0]   ctrl_a_addr;
    logic [$clog2(SP_B_DEPTH)-1:0]   ctrl_b_addr;
    logic [$clog2(SP_C_DEPTH)-1:0]   ctrl_c_addr;
    logic                            ctrl_c_we;
    logic [C_WIDTH-1:0]              ctrl_c_wdata;

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

    // ── Scratchpad port muxing ──────────────────────────────────────────
    // When controller is not running: external port drives scratchpads
    // When controller is running: controller drives scratchpads

    // SP_A: controller reads only, external writes only
    always_comb begin
        if (ctrl_running) begin
            sp_a_addr  = ctrl_a_addr;
            sp_a_we    = 1'b0;
            sp_a_wdata = '0;
        end else begin
            sp_a_addr  = ext_addr[$clog2(SP_A_DEPTH)-1:0];
            sp_a_we    = ext_we && (ext_sel == 2'd0);
            sp_a_wdata = ext_wdata[A_WIDTH-1:0];
        end
    end

    // SP_B: controller reads only, external writes only
    always_comb begin
        if (ctrl_running) begin
            sp_b_addr  = ctrl_b_addr;
            sp_b_we    = 1'b0;
            sp_b_wdata = '0;
        end else begin
            sp_b_addr  = ext_addr[$clog2(SP_B_DEPTH)-1:0];
            sp_b_we    = ext_we && (ext_sel == 2'd1);
            sp_b_wdata = ext_wdata[B_WIDTH-1:0];
        end
    end

    // SP_C: controller reads and writes, external reads and writes
    always_comb begin
        if (ctrl_running) begin
            sp_c_addr  = ctrl_c_addr;
            sp_c_we    = ctrl_c_we;
            sp_c_wdata = ctrl_c_wdata;
        end else begin
            sp_c_addr  = ext_addr[$clog2(SP_C_DEPTH)-1:0];
            sp_c_we    = ext_we && (ext_sel == 2'd2);
            sp_c_wdata = ext_wdata[C_WIDTH-1:0];
        end
    end

    // External read data mux
    always_comb begin
        case (ext_sel)
            2'd0:    ext_rdata = {{(C_WIDTH-A_WIDTH){1'b0}}, sp_a_rdata};
            2'd1:    ext_rdata = {{(C_WIDTH-B_WIDTH){1'b0}}, sp_b_rdata};
            2'd2:    ext_rdata = sp_c_rdata;
            default: ext_rdata = '0;
        endcase
    end

    // ── Scratchpad instances ────────────────────────────────────────────

    scratchpad #(
        .DEPTH(SP_A_DEPTH),
        .WIDTH(A_WIDTH)
    ) sp_a (
        .clk(clk),
        .we(sp_a_we),
        .addr(sp_a_addr),
        .wdata(sp_a_wdata),
        .rdata(sp_a_rdata)
    );

    scratchpad #(
        .DEPTH(SP_B_DEPTH),
        .WIDTH(B_WIDTH)
    ) sp_b (
        .clk(clk),
        .we(sp_b_we),
        .addr(sp_b_addr),
        .wdata(sp_b_wdata),
        .rdata(sp_b_rdata)
    );

    scratchpad #(
        .DEPTH(SP_C_DEPTH),
        .WIDTH(C_WIDTH)
    ) sp_c (
        .clk(clk),
        .we(sp_c_we),
        .addr(sp_c_addr),
        .wdata(sp_c_wdata),
        .rdata(sp_c_rdata)
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
        .sp_a_rdata(sp_a_rdata),
        .sp_b_addr(ctrl_b_addr),
        .sp_b_rdata(sp_b_rdata),
        .sp_c_addr(ctrl_c_addr),
        .sp_c_we(ctrl_c_we),
        .sp_c_wdata(ctrl_c_wdata),
        .sp_c_rdata(sp_c_rdata),
        .arr_enable(arr_enable),
        .arr_load_weight(arr_load_weight),
        .arr_a_in(arr_a_in),
        .arr_b_in(arr_b_in),
        .arr_drain_out(arr_drain_out)
    );

endmodule
