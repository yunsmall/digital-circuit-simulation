// dual_clk.v — 伪时钟测试用：两个时钟域，一个上升沿、一个下降沿
module dual_clk(
    input clk_a,
    input clk_b,
    input rst,
    output reg [7:0] count_a,
    output reg [7:0] count_b
);
always @(posedge clk_a)
    if (rst) count_a <= 8'd0;
    else count_a <= count_a + 8'd1;

always @(negedge clk_b)
    if (rst) count_b <= 8'd0;
    else count_b <= count_b + 8'd1;
endmodule
