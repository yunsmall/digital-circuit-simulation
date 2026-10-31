// wide128 — 128 位直通，用来验证宽端口（线网槽 16 字节）的生成与仿真
module wide128 (
    input  [127:0] a,
    output [127:0] y
);
    assign y = a;
endmodule
