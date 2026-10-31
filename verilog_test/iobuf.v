// iobuf — 双向 pad 的驱动侧：oe=1 时把 dout 推上 pad，oe=0 时高阻
module iobuf (
    input  [7:0] dout,
    input        oe,
    inout  [7:0] pad
);
    assign pad = oe ? dout : 8'bz;
endmodule
