/* The synthesiser as a flat image inside SB.MOD.
   The address here must match where the module places the incbin, and the
   module's own ORG is added to neither: the module's selectors are based at
   the module, so an offset within it is all any address needs to be.

   Two modules place it, and their code is not the same size, so the address
   is not written here but given on the command line - see tools/build_opl.py.
   SB.MOD has it low, where its own code ends; DPMI.MOD, which is larger,
   has it above everything of its own. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(opl_reset)
SECTIONS
{
    . = @OPL_AT@;               /* tools/build_opl.py fills this in */
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
