OUTPUT_ARCH( "riscv" )
ENTRY(_start)
SECTIONS
{
  . = 0x0;
  .text.init : { *(.text.init) }
  .text : { *(.text) }
  . = ALIGN(0x100);
  .tohost : { *(.tohost) }
  .data : { *(.data) }
  .bss : { *(.bss) }
  _end = .;
}
