// counter8 — 8 位计数器，带使能和复位
module counter8 (
    input        clk,
    input        rst,   // 同步复位，高有效
    input        en,    // 使能
    output [7:0] count
);
    reg [7:0] cnt;
    assign count = cnt;

    always @(posedge clk) begin
        if (rst)
            cnt <= 0;
        else if (en)
            cnt <= cnt + 1;
    end
endmodule
