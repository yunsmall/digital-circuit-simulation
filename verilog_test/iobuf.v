// iobuf — 双向 pad：oe=1 时把 dout 推上 pad，oe=0 时高阻；din 读 pad 上的实际值
module iobuf (
    input  [7:0] dout,
    input        oe,
    inout  [7:0] pad,
    output [7:0] din
);
    assign pad = oe ? dout : 8'bz;
    assign din = pad;
endmodule
