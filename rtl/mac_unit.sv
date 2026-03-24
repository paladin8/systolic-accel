module mac_unit #(
    parameter DATA_WIDTH = 16,
    parameter ACC_WIDTH  = 32
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  enable,
    input  logic                  clear_acc,
    input  logic [DATA_WIDTH-1:0] a,
    input  logic [DATA_WIDTH-1:0] b,
    output logic [DATA_WIDTH-1:0] a_out,
    output logic [DATA_WIDTH-1:0] b_out,
    output logic [ACC_WIDTH-1:0]  result
);

    logic [ACC_WIDTH-1:0] mult_reg;
    logic [ACC_WIDTH-1:0] acc_reg;

    // Stage 1: registered multiplier output + passthrough
    // Synthesizes to: mult_reg[ACC_WIDTH] + a_out[DATA_WIDTH] + b_out[DATA_WIDTH] flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mult_reg <= '0;
            a_out    <= '0;
            b_out    <= '0;
        end else if (enable) begin
            mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(b));
            a_out    <= a;
            b_out    <= b;
        end
    end

    // Stage 2: accumulator register
    // Synthesizes to: acc_reg[ACC_WIDTH] flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc_reg <= '0;
        end else if (enable) begin
            if (clear_acc)
                acc_reg <= mult_reg;
            else
                acc_reg <= acc_reg + mult_reg;
        end
    end

    assign result = acc_reg;

endmodule
