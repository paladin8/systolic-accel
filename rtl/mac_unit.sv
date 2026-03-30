module mac_unit #(
    parameter DATA_WIDTH     = 16,
    parameter ACC_WIDTH      = 32,
    parameter PIPELINE_DEPTH = 2   // 1, 2, or 3
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  enable,
    input  logic                  load_weight,
    input  logic [DATA_WIDTH-1:0] a,
    input  logic [DATA_WIDTH-1:0] b,
    output logic [DATA_WIDTH-1:0] a_out,
    output logic [DATA_WIDTH-1:0] b_out,
    input  logic [ACC_WIDTH-1:0]  psum_in,
    output logic [ACC_WIDTH-1:0]  psum_out
);

    logic [DATA_WIDTH-1:0] weight_reg;

    // Weight register — loaded during weight phase, stationary during compute
    // Synthesizes to: DATA_WIDTH flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            weight_reg <= '0;
        else if (enable && load_weight)
            weight_reg <= b;
    end

    // Passthrough registers — always stage 1, all depths
    // Synthesizes to: 2 * DATA_WIDTH flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_out <= '0;
            b_out <= '0;
        end else if (enable) begin
            a_out <= a;
            b_out <= b;
        end
    end

    // Pipeline registers — declared at module scope so formal properties
    // can reference them directly. Unused registers (e.g., mult_reg at
    // depth 1) are optimized away by synthesis.
    logic [ACC_WIDTH-1:0] mult_reg;
    logic [ACC_WIDTH-1:0] add_reg;

    generate
        if (PIPELINE_DEPTH == 1) begin : gen_depth1
            // Depth 1: combinational multiply-add, registered at psum_out
            // Synthesizes to: ACC_WIDTH flip-flops (psum_out only)
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    psum_out <= '0;
                else if (enable)
                    psum_out <= psum_in + ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            end

        end else if (PIPELINE_DEPTH == 2) begin : gen_depth2
            // Depth 2: register after multiply, then add
            // Synthesizes to: mult_reg[ACC_WIDTH] + psum_out[ACC_WIDTH] flip-flops
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    mult_reg <= '0;
                else if (enable)
                    mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    psum_out <= '0;
                else if (enable)
                    psum_out <= psum_in + mult_reg;
            end

        end else if (PIPELINE_DEPTH == 3) begin : gen_depth3
            // Depth 3: register after multiply, register after add, then output
            // Synthesizes to: mult_reg[ACC_WIDTH] + add_reg[ACC_WIDTH] + psum_out[ACC_WIDTH] flip-flops
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    mult_reg <= '0;
                else if (enable)
                    mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    add_reg <= '0;
                else if (enable)
                    add_reg <= psum_in + mult_reg;
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    psum_out <= '0;
                else if (enable)
                    psum_out <= add_reg;
            end
        end
    endgenerate

// ── Formal verification ───────────────────────────────────────────
`ifdef FORMAL
    // -- Common infrastructure --
    logic f_past_valid;
    initial f_past_valid = 1'b0;
    always_ff @(posedge clk)
        f_past_valid <= 1'b1;

    // Reset assumption: start in reset, once released stay released
    initial assume (!rst_n);
    always_ff @(posedge clk)
        if (f_past_valid && $past(rst_n))
            assume (rst_n);

    // ── MAC1: Weight latch ────────────────────────────────────────
    // When enable && load_weight were high last cycle, weight_reg
    // captures b from last cycle
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && $past(rst_n && enable && load_weight))
            mac1: assert (weight_reg == $past(b));

    // ── MAC2: Weight stability ────────────────────────────────────
    // When enable && load_weight were NOT both high, weight_reg holds
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && $past(rst_n && !(enable && load_weight)))
            mac2: assert (weight_reg == $past(weight_reg));

    // ── MAC3: Reset clears all registers ──────────────────────────
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && !$past(rst_n)) begin
            mac3_w: assert (weight_reg == '0);
            mac3_a: assert (a_out == '0);
            mac3_b: assert (b_out == '0);
            mac3_p: assert (psum_out == '0);
        end

    // ── MAC4: Activation forwarding ───────────────────────────────
    // One-cycle delay: a_out = previous cycle's a (when enabled)
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && $past(rst_n && enable))
            mac4: assert (a_out == $past(a));

    // ── MAC5: Weight forwarding ───────────────────────────────────
    // One-cycle delay: b_out = previous cycle's b (when enabled)
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && $past(rst_n && enable))
            mac5: assert (b_out == $past(b));

    // ── MAC6: Stall hold ──────────────────────────────────────────
    // When !enable, passthrough registers hold their values
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && $past(rst_n && !enable)) begin
            mac6_a: assert (a_out == $past(a_out));
            mac6_b: assert (b_out == $past(b_out));
        end

    // ── MAC7/8/9: Compute correctness (depth-dependent) ──────────
    // mult_reg and add_reg are declared at module scope, so we can
    // reference them directly — no shadow registers needed.
    //
    // Depth 1: psum_out = psum_in + a * weight_reg, 1-cycle latency
    // Depth 2: mult_reg = a * weight_reg; psum_out = psum_in + mult_reg
    // Depth 3: mult_reg → add_reg → psum_out

    if (PIPELINE_DEPTH == 1) begin : gen_formal_d1
        // Depth 1: single-cycle multiply-accumulate
        always_ff @(posedge clk)
            if (f_past_valid && rst_n && $past(rst_n && enable))
                mac7: assert (psum_out == $past(
                    psum_in + ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg))
                ));
    end else if (PIPELINE_DEPTH == 2) begin : gen_formal_d2
        // Depth 2: verify mult stage, then add stage
        always_ff @(posedge clk)
            if (f_past_valid && rst_n && $past(rst_n && enable))
                mac8_mult: assert (mult_reg == $past(
                    ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg))
                ));

        always_ff @(posedge clk)
            if (f_past_valid && rst_n && $past(rst_n && enable))
                mac8_add: assert (psum_out == $past(psum_in + mult_reg));
    end else if (PIPELINE_DEPTH == 3) begin : gen_formal_d3
        // Depth 3: verify each pipeline stage
        always_ff @(posedge clk)
            if (f_past_valid && rst_n && $past(rst_n && enable))
                mac9_mult: assert (mult_reg == $past(
                    ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg))
                ));

        always_ff @(posedge clk)
            if (f_past_valid && rst_n && $past(rst_n && enable))
                mac9_add: assert (add_reg == $past(psum_in + mult_reg));

        always_ff @(posedge clk)
            if (f_past_valid && rst_n && $past(rst_n && enable))
                mac9_out: assert (psum_out == $past(add_reg));
    end

    // ── MAC6b: Stall hold for psum_out ────────────────────────────
    // psum_out holds when !enable (all depths)
    always_ff @(posedge clk)
        if (f_past_valid && rst_n && $past(rst_n && !enable))
            mac6_p: assert (psum_out == $past(psum_out));

    // ── MAC10: Cover — nonzero computation ────────────────────────
    always_ff @(posedge clk)
        if (f_past_valid && rst_n)
            mac10: cover (psum_out != '0 && weight_reg != '0
                         && a_out != '0 && psum_in != '0);
`endif

endmodule
