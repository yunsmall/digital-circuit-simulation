OUTPUT_ARCH( "riscv" )
ENTRY(_start)
SECTIONS
{
  /* 官方测试假定代码/数据位于 DRAM_BASE(0x80000000)。dirty.S 用
     "符号 - DRAM_BASE" 计算虚拟地址，只有链接在 DRAM_BASE 时这个差值才是它
     期望的低地址（链接在 0 会让 32 位减法回绕成 0x80001008）。
     仿真器的 WishboneMemory 只接低 addr_width 根地址线，高地址会自动别名回来，
     所以链接在 0x80000000 不影响映像的实际存放位置。 */
  . = 0x80000000;
  .text.init : { *(.text.init) }
  .text : { *(.text) }
  . = ALIGN(0x100);
  .tohost : { *(.tohost) }
  .data : { *(.data) }
  .bss : { *(.bss) }
  _end = .;
}
