; =============================================================================
;  retro.asm - RETRO.COM: the boot that never quite worked, for the camera
; -----------------------------------------------------------------------------
;  The video needs a "before": the first weeks, when the machine ran our code
;  badly.  No footage of that exists, so this is a dramatisation - a boot
;  that scrolls three screens of probes, retries and corruption, slowly, and
;  then asks for a key.  It changes nothing and reads nothing; every line is
;  written here.  Only the NanoDOS image ships it (build.py --retro), and
;  AUTOEXEC.BAT runs it before the prompt.
; =============================================================================
[BITS 16]
[ORG 0x100]

start:
        mov     word [seed], 0x2A3F
        mov     si, script
.line:  lodsb
        cmp     al, 0xFF                ; the end
        je      .done
        cmp     al, 0xFE                ; garbage: next byte = how many chars
        je      .garbage
        cmp     al, 0xFD                ; a pause: next byte = ticks
        je      .pause
        cmp     al, 0xFC                ; a slow line: the rest one char at a time
        je      .slow
        dec     si
        call    say                     ; a plain line, then a short wait
        mov     cx, 2
        call    wait_ticks
        jmp     .line
.garbage:
        lodsb
        mov     cl, al
        call    garbage_line
        mov     cx, 1
        call    wait_ticks
        jmp     .line
.pause: lodsb
        movzx   cx, al
        call    wait_ticks
        jmp     .line
.slow:  call    say_slow
        jmp     .line
.done:  mov     si, msg_key
        call    say
        xor     ah, ah
        int     0x16
        mov     si, crlf
        call    say
        mov     ax, 0x4C00
        int     0x21

; say: DS:SI to the screen, up to a NUL, then a new line
say:    push    ax
        push    bx
.next:  lodsb
        or      al, al
        jz      .eol
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     .next
.eol:   mov     al, 13
        mov     ah, 0x0E
        int     0x10
        mov     al, 10
        int     0x10
        pop     bx
        pop     ax
        ret

; say_slow: the same, a character every tick, like something struggling
say_slow:
        push    ax
        push    bx
        push    cx
.next:  lodsb
        or      al, al
        jz      .eol
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        mov     cx, 1
        call    wait_ticks
        jmp     .next
.eol:   mov     al, 13
        mov     ah, 0x0E
        int     0x10
        mov     al, 10
        int     0x10
        pop     cx
        pop     bx
        pop     ax
        ret

; garbage_line: CL characters of nothing in particular
garbage_line:
        push    ax
        push    bx
        push    cx
        movzx   cx, cl
.next:  call    rnd
        and     al, 0x7F
        add     al, 0x21                ; 21h..A0h: punctuation, letters, box bits
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        loop    .next
        mov     al, 13
        mov     ah, 0x0E
        int     0x10
        mov     al, 10
        int     0x10
        pop     cx
        pop     bx
        pop     ax
        ret

; rnd: AL = the next of a linear congruential sequence
rnd:    push    dx
        mov     ax, [seed]
        mov     dx, 25173
        mul     dx
        add     ax, 13849
        mov     [seed], ax
        mov     al, ah
        pop     dx
        ret

; wait_ticks: CX ticks of the clock, the way a BIOS counts them
wait_ticks:
        push    ax
        push    bx
        push    cx
        push    dx
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ebx, [es:0x46C]
.wait:  hlt
        mov     eax, [es:0x46C]
        sub     eax, ebx
        cmp     ax, cx
        jb      .wait
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; The script.  FCh: the line comes out one character at a time.  FEh n: n
; characters of garbage.  FDh n: wait n ticks.  FFh: the end.
; -----------------------------------------------------------------------------
script:
        db 0xFC, "NanoDOS 0.1 initializing...", 0
        db 0xFD, 18
        db "Probing ISA bus.", 0
        db "  Port 0378h: IRQ 7 conflict", 0
        db "  Port 02F8h: no device", 0
        db "  Port 0220h: no device", 0
        db "  Port 0330h: no device", 0
        db 0xFD, 9
        db "Memory test: 640K OK", 0
        db 0xFC, "Memory test: 8192K ....", 0
        db "  FAIL at 00A3:F000 (expected 55h, read D5h)", 0
        db 0xFC, "  Retrying (1).....", 0
        db 0xFC, "  Retrying (2).....", 0
        db "  Skipping bank 1", 0
        db 0xFD, 9
        db "A20 gate: no response from keyboard controller", 0
        db "A20 gate: trying port 92h", 0
        db "A20 gate: OK", 0
        db 0xFE, 62
        db 0xFE, 71
        db 0xFE, 34
        db 0xFD, 12
        db "Drive C: reading boot sector", 0
        db "Sector 0002A3Fh: CRC error", 0
        db "Sector 0002A3Fh: CRC error", 0
        db "Sector 0002A3Fh: CRC error", 0
        db 0xFC, "Retrying.........", 0
        db "Sector 0002A3Fh: OK", 0
        db "FAT: 2 copies differ; using the first", 0
        db 0xFD, 9
        db "Loading NANODOS.SYS ......................", 0
        db "Loading MOUSE.SYS", 0
        db "  No mouse detected", 0
        db "Loading SOUND.SYS", 0
        db "  No Sound Blaster at 220h", 0
        db "  No Sound Blaster at 240h", 0
        db "  No Sound Blaster at 260h", 0
        db "Loading VESA.SYS", 0
        db "  VESA BIOS: 0 modes", 0
        db 0xFE, 58
        db 0xFE, 12
        db 0xFE, 77
        db 0xFE, 40
        db 0xFE, 66
        db 0xFD, 12
        db "Warning: interrupt vector 08h taken by unknown code", 0
        db "Warning: interrupt vector 09h taken by unknown code", 0
        db "Stack overflow in module CONSOLE", 0
        db 0xFC, "Recovering.............", 0
        db "Divide overflow at 0800:1C3E", 0
        db "  AX=0000 BX=7C00 CX=FFFF DX=0080", 0
        db "  Continuing anyway", 0
        db 0xFE, 45
        db 0xFE, 80
        db 0xFE, 23
        db 0xFE, 69
        db 0xFE, 51
        db 0xFD, 9
        db "Timer: 18.2 Hz expected, 0.0 Hz measured", 0
        db "Timer: reprogramming", 0
        db "Keyboard: 8042 self-test failed (AAh expected, FFh read)", 0
        db "Keyboard: continuing without", 0
        db 0xFD, 9
        db "Video: 80x25 colour", 0
        db "Video: 320x200 not available", 0
        db "Video: 640x480 not available", 0
        db 0xFE, 30
        db 0xFE, 74
        db 0xFD, 6
        db 0xFC, "NanoDOS 0.1 ready.", 0
        db 0xFD, 9
        db 0xFF

msg_key:        db "Press any key to continue.", 0
crlf:           db 0
seed:           dw 0
