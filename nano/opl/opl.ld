/* The synthesiser as a flat image inside SB.MOD.
   The address here must match where modules/sb.asm places the incbin, and
   SB.MOD's own ORG is added to neither: the module's selectors are based at
   the module, so an offset within it is all any address needs to be. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(opl_reset)
SECTIONS
{
    . = 0x6000;
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    . = ALIGN(16);
    __opl_image_end = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __opl_end = .;
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) *(.eh_frame_hdr) *(.rel.*) }
}
