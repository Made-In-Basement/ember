/* Ember NX32 flat image: header first, then code and data, bss after */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    .nxhdr  : { KEEP(*(.nxhdr)) }
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    . = ALIGN(16);
    __data_end = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) *(.eh_frame_hdr) *(.rel.*) }
}
