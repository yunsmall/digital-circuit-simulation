OUTPUT_ARCH( "riscv" )
ENTRY(_start)
SECTIONS
{
  . = 0x0;
  .text.init : { *(.text.init) }
  .text : { *(.text) }
  . = ALIGN(0x100);
  .tohost : { tohost = .; . += 8; *(.tohost) }   /* 必须占住 8 字节，否则 .bss 会从这里开始覆盖 tohost */
  .rodata : { *(.rodata*) }
  .data : { *(.data) }
  .bss : { *(.bss) *(COMMON) }
  _end = .;
}
