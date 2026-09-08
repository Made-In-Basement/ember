; =============================================================================
;  ember.inc - what a resident module needs to know about the kernel
; -----------------------------------------------------------------------------
;  A module is a flat binary, ORG 0, loaded into a segment of its own that
;  belongs to the system.  It starts with the 32-byte header MODULE_HEADER
;  lays out, and is entered by far calls from the kernel (src/module.asm):
;
;     init    DS = this segment, ES:SI = the rest of the LOAD line.
;             Return CF=1 to refuse; the memory goes straight back.
;     unload  DS = this segment.  CF=1 refuses (something else has hooked a
;             vector behind us, say).  Only the last module loaded is asked.
;     event   DS = this segment, AL = one of the MOD_EV_ values.
;
;  Kernel services are far calls through a table the module copies into its
;  own segment at init (SVC_TABLE_COPY), then SVC n reaches service n.
; =============================================================================

MOD_EV_PROMPT   equ 0                           ; the shell is at its prompt
MOD_EV_START    equ 1                           ; a program is about to run
MOD_EV_END      equ 2                           ; the program has ended

; ---- services, by number ----
SVC_PUTS        equ 0                           ; DS:SI NUL-terminated string
SVC_PUTC        equ 1                           ; AL
SVC_CRLF        equ 2
SVC_PRINT_DEC   equ 3                           ; EAX
SVC_PRINT_HEX16 equ 4                           ; AX
SVC_MEM_ALLOC   equ 5                           ; BX paragraphs -> AX segment, CF=1 none
SVC_MEM_FREE    equ 6                           ; ES = block
SVC_MEM_RESIZE  equ 7                           ; ES = block, BX = paragraphs, CF=1 no
SVC_EXT_MEM_END equ 8                           ; -> EAX physical end of extended memory
SVC_LOG_TEXT    equ 9                           ; DS:SI line into EMBER.LOG
SVC_LOG_LINE    equ 10                          ; DS:SI text, then EAX in hex
SVC_GETKEY      equ 11                          ; -> AX (BIOS scan code, ASCII)
SVC_KBHIT       equ 12                          ; -> ZF clear when a key waits
SVC_DELAY_TICKS equ 13                          ; CX ticks of 55 ms
SVC_MEM_LARGEST equ 14                          ; -> BX paragraphs
SVC_PRINT_DEC_PAD equ 15                        ; EAX right-aligned in CL columns
SVC_MAX         equ 32                          ; room kept for the copy

; ---- the header: name is 8 characters, upper case, space padded ----
;   MODULE_HEADER name, init, unload, event, paragraphs, resident
;   paragraphs: memory wanted including zeroed space (0 = the file's size)
;   resident:   paragraphs kept after init (0 = all of it)
%macro MODULE_HEADER 6
        db      "EMOD"
        dw      1
        dw      0
        db      %1
        dw      %2
        dw      %3
        dw      %4
        dw      %5
        dw      %6
mod_services_ptr: dd 0                          ; the loader fills this in
        dw      0
%endmacro

; ---- calling a service: SVC number ----
%macro SVC 1
        call    far [cs:svc_table + (%1) * 4]
%endmacro

; ---- SVC_TABLE_COPY: at init, bring the kernel's table over (keeps nothing) ----
%macro SVC_TABLE_COPY 0
        push    ds
        push    es
        push    si
        push    di
        push    cx
        push    ax
        mov     ax, ds
        mov     es, ax
        lds     si, [mod_services_ptr]
        lodsw                                   ; the count
        cmp     ax, SVC_MAX
        jbe     %%count_ok
        mov     ax, SVC_MAX
%%count_ok:
        mov     cx, ax
        shl     cx, 1                           ; words
        mov     di, svc_table
        rep     movsw
        pop     ax
        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds
%endmacro
