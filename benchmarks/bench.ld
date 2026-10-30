OUTPUT_ARCH( "riscv" )
ENTRY(_start)
SECTIONS
{
  . = 0x0;
  .text.init : { *(.text.init) }
  .text : { *(.text) }
  . = ALIGN(0x100);
  .tohost : { tohost = .; *(.tohost) }
  .rodata : { *(.rodata*) }
  .data : { *(.data) }
  .bss : { *(.bss) *(COMMON) }
  _end = .;
}
