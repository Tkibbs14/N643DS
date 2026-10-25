/* CPU test cartridge: code at the standard entry point (the high-level boot copies 1 MB there). */
ENTRY(_start)
SECTIONS
{
    . = 0x80000400;
    .text : { *crt.o(.text) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) }
    __bss_start = .;
    .bss : { *(.sbss*) *(.bss*) *(COMMON) }
    . = ALIGN(4);
    _end = .;
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
