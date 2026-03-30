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

// ── Formal verification ───────────────────────────────────────────
`ifdef FORMAL
    // Shadow register technique: track mem[f_addr] via a formal-only
    // register. The solver picks f_addr; we shadow all writes to it.
    // No rst_n on scratchpad — memory starts unconstrained.

    (* anyconst *) logic [$clog2(DEPTH)-1:0] f_addr;
    logic [WIDTH-1:0] f_shadow;
    logic f_written;      // have we seen at least one write to f_addr?
    logic f_past_valid;

    initial begin
        f_past_valid = 1'b0;
        f_written    = 1'b0;
    end

    always_ff @(posedge clk) begin
        f_past_valid <= 1'b1;
        if (we && addr == f_addr) begin
            f_shadow <= wdata;
            f_written <= 1'b1;
        end
    end

    // SP1: Read-after-write — reading f_addr returns the last written value
    always_ff @(posedge clk)
        if (f_past_valid && f_written && $past(addr == f_addr && !we))
            sp1: assert (rdata == f_shadow);

    // SP2: Read stability — if no writes for two consecutive cycles and
    //      addr stable, rdata is stable. Need !we at cycles N, N-1, AND
    //      N-2 because rdata is registered: a write at N-2 changes mem,
    //      which changes rdata at N-1 vs N.
    always_ff @(posedge clk)
        if (f_past_valid && $past(f_past_valid) && !we && !$past(we)
            && !$past(we, 2) && addr == $past(addr) && $past(addr) == $past(addr, 2))
            sp2: assert (rdata == $past(rdata));

    // SP3: Write isolation — writing to a different address doesn't
    //      change the value stored at f_addr
    always_ff @(posedge clk)
        if (f_past_valid && f_written && $past(we && addr != f_addr))
            sp3: assert (f_shadow == $past(f_shadow));

    // SP4: Read-first — on a write cycle to f_addr, rdata reflects
    //      the OLD value (before the write takes effect).
    //      Gate on $past(f_written) so f_shadow was valid before this write.
    always_ff @(posedge clk)
        if (f_past_valid && $past(f_written && we && addr == f_addr))
            sp4: assert (rdata == $past(f_shadow));

    // SP5: Cover — solver finds a trace: write to f_addr, then read back
    always_ff @(posedge clk)
        if (f_past_valid)
            sp5: cover (f_written && addr == f_addr && !we
                        && rdata == f_shadow);
`endif

endmodule
