// Generic synchronous single-port SRAM.
// Read-first: rdata reflects the value at addr before any same-cycle write.
module scratchpad #(
    parameter DEPTH = 64,
    parameter WIDTH = 64
)(
    input  logic                     clk,
    input  logic                     we,
    input  logic [$clog2(DEPTH)-1:0] addr,
    input  logic [WIDTH-1:0]         wdata,
    output logic [WIDTH-1:0]         rdata
);

    // Synthesizes to: DEPTH x WIDTH bits of register-based SRAM
    logic [WIDTH-1:0] mem [0:DEPTH-1];

    always_ff @(posedge clk) begin
        rdata <= mem[addr];
        if (we)
            mem[addr] <= wdata;
    end

endmodule
