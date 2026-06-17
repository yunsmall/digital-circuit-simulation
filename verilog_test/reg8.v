// reg8 — 8 位寄存器：组合直通 + 时序寄存
module reg8 (
    input        clk,
    input  [7:0] d,
    output [7:0] q,      // 时序输出（clk 上升沿锁存）
    output [7:0] d_out    // 组合输出（直接透传 d）
);
    reg [7:0] q_reg;
    assign q = q_reg;
    assign d_out = d;

    always @(posedge clk) begin
        q_reg <= d;
    end
endmodule
