; =============================================================================
;  Ember kernel  (src/kernel.asm)
; -----------------------------------------------------------------------------
;  A single 16-bit real-mode binary loaded by the boot sector at 0800:0000.
;  Entered with DL = BIOS boot drive, EBX = LBA of the volume on the disk.
;
;  Memory map (physical):
;     0x00000  interrupt vector table / BIOS data
;     0x08000  this kernel (code, data, bss, stack up to 0x17FFF)
;     0x18000  DOS memory arena for programs (MCB blocks, see mem.asm)
;     top      GUI scratch, HD Audio buffers and the boot log (mem.asm)
;     0xA0000  video memory
; =============================================================================

[BITS 16]
[ORG 0x0000]

%define VERSION "1.2"
%include "../build/version.inc"
; The NanoDOS image (build.py --retro) is a dramatisation of the first
; weeks, for the video: the same kernel under the old name, with the old
; desktop throwing errors.  Nothing about it is real and nothing else
; changes; OS_NAME is what every banner says.
%ifdef NANODOS
%define OS_NAME "NanoDOS"
%define OS_VER "0.1"
%define OS_DESKTOP "WIN"
%else
%define OS_NAME "Ember"
%define OS_VER VERSION
%define OS_DESKTOP "EMBER"
%endif
KERNEL_SEG      equ 0x0800
BATCH_MAX       equ 2047
AFTER_MAX       equ 512                         ; lines a program leaves to run after it

section .text
start:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0xFFFE
        sti
        cld
        mov     [boot_drive], dl
        mov     [fs_hidden], ebx

        ; ---- zero the uninitialised data area ----
        mov     di, bss_start
        mov     cx, bss_end - bss_start
        xor     al, al
        rep     stosb

        call    install_interrupts
        call    disk_init
        call    fs_init
        call    mem_init
        call    log_reset
        mov     si, msg_log_header
        call    log_text
        movzx   eax, byte [boot_drive]
        mov     si, msg_log_drive
        call    log_line

        mov     al, 'A'
        mov     byte [drive_number], 0
        cmp     byte [boot_drive], 0x80
        jb      .letter
        mov     al, 'C'
        mov     byte [drive_number], 2
.letter:
        mov     [drive_letter], al

        call    set_text_mode
        call    cls
        mov     si, msg_banner
        call    puts
        cmp     byte [fs_ok], 0
        jne     .fs_fine
        mov     si, msg_no_fs_warn
        call    puts
.fs_fine:
        ; Holding a Shift key while booting skips AUTOEXEC.BAT.  Give the user
        ; about 1.5 seconds to press it (polled every timer tick).
        mov     si, msg_starting
        call    puts
        mov     cx, 27
.shift_poll:
        mov     ah, 0x02
        int     0x16
        test    al, 0x03
        jnz     .skip_autoexec
        push    cx
        mov     cx, 1
        call    delay_ticks
        pop     cx
        loop    .shift_poll
        call    crlf
        call    run_autoexec
        jmp     shell_main
.skip_autoexec:
        mov     si, msg_autoexec_skipped
        call    puts
        jmp     shell_main

section .bss
bss_start:
section .text

%include "console.asm"
%include "disk.asm"
%include "fat.asm"
%include "fatlfn.asm"
%include "fatwrite.asm"
%include "fatdir.asm"
%include "mem.asm"
%include "dos.asm"
%include "shell.asm"
%include "complete.asm"
%include "log.asm"
%include "sound.asm"
%include "spkbrdg.asm"
%include "splash.asm"
%include "gfx.asm"
%include "mouse.asm"
%include "icons.asm"
%include "gui.asm"
%include "apps.asm"
%include "pm32.asm"
%include "module.asm"

section .data
boot_drive:     db 0
drive_letter:   db 'A'
drive_number:   db 0
screen_attr:    db 0x07
msg_banner:
        db OS_NAME, " Version ", OS_VER, " (", BUILD_STAMP, ")", 13, 10
        db "A DOS-like operating system written from scratch in x86 assembly.", 13, 10
        db "Type HELP for a list of commands, or ", OS_DESKTOP, " for the desktop.", 13, 10
        db 13, 10, 0
msg_no_fs_warn:
        db "Warning: no valid FAT filesystem found on the boot disk.", 13, 10
        db "File commands (DIR, TYPE, CD, programs) will not work.", 13, 10, 13, 10, 0
msg_starting:
        db "Starting ", OS_NAME, " (hold Shift to skip AUTOEXEC.BAT)...", 0
msg_log_header:
        db OS_NAME, " ", OS_VER, " boot log (written to EMBER.LOG after sound commands and the GUI)", 0
msg_log_drive:
        db "BIOS boot drive         ", 0
msg_autoexec_skipped:
        db 13, 10, "AUTOEXEC.BAT skipped (Shift held).", 13, 10, 0

section .bss
bss_end:

section .data
kernel_top_marker: dd 0x4B4C4159, bss_start, bss_end, kernel_bss_end   ; "YALK": build.py checks the layout
section .bss
kernel_bss_end:
section .text
