; =============================================================================
;  ADLIB.COM - four notes through a synthesiser that is not there
; -----------------------------------------------------------------------------
;  An AdLib card is two ports: 388h takes a register number and 389h takes
;  what goes in it.  A program finds one by resetting the chip's timer flags,
;  reading the status (nothing set), starting a timer, waiting, and reading
;  again (both set) - and that is all the handshake there is.
;
;  This does that, sets up one two-operator voice, and plays a rising phrase.
;  On a machine with no card it says so.  With SB.MOD loaded there is still no
;  card, but the program is in virtual-8086 mode and the monitor answers for
;  one, synthesises what it is told, and mixes the result into the machine's
;  own audio stream.
; =============================================================================

[BITS 16]
[ORG 0x100]

FM_ADDR         equ 0x388
FM_DATA         equ 0x389

start:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld
        mov     dx, msg_title
        call    puts

; ---- the handshake ----------------------------------------------------------
        mov     al, 0x04                        ; timer control
        mov     ah, 0x60                        ; hold both timers
        call    fm_out
        mov     al, 0x04
        mov     ah, 0x80                        ; and reset their flags
        call    fm_out
        mov     dx, FM_ADDR
        in      al, dx
        mov     bl, al                          ; should be nothing at all
        mov     al, 0x02                        ; timer 1's count
        mov     ah, 0xFF
        call    fm_out
        mov     al, 0x04
        mov     ah, 0x21                        ; let timer 1 run
        call    fm_out
        mov     cx, 400
.wait:  in      al, 0x80
        loop    .wait
        mov     dx, FM_ADDR
        in      al, dx
        mov     bh, al                          ; should be both flags now
        mov     al, 0x04
        mov     ah, 0x60
        call    fm_out
        mov     al, 0x04
        mov     ah, 0x80
        call    fm_out

        mov     dx, msg_status
        call    puts
        mov     al, bl
        call    put_hex8
        mov     dx, msg_then
        call    puts
        mov     al, bh
        call    put_hex8
        call    crlf
        and     bl, 0xE0
        jnz     .nothing
        and     bh, 0xE0
        cmp     bh, 0xC0
        jne     .nothing
        mov     dx, msg_found
        call    puts
        jmp     .voice
.nothing:
        mov     dx, msg_none
        call    puts
        jmp     leave

; ---- one voice, and a phrase through it ------------------------------------
.voice:
        mov     si, patch
.set:   lodsw                                   ; AL = register, AH = value
        or      al, al
        jz      .patched
        call    fm_out
        jmp     .set
.patched:
        mov     dx, msg_playing
        call    puts
        mov     si, notes
.note:  lodsw
        or      ax, ax
        jz      .done
        mov     bx, ax                          ; BX = fnum | block<<10
        mov     al, 0xA0                        ; the low eight bits of it
        mov     ah, bl
        call    fm_out
        mov     ax, bx
        shr     ax, 8
        and     al, 0x1F                        ; block and the top two bits
        mov     ah, al
        or      ah, 0x20                        ; and the key, pressed
        mov     al, 0xB0
        call    fm_out
        mov     cx, 7
        call    wait_ticks
        mov     ax, bx                          ; and let go: the same, no key
        shr     ax, 8
        and     al, 0x1F
        mov     ah, al
        mov     al, 0xB0
        call    fm_out
        mov     cx, 2
        call    wait_ticks
        jmp     .note
.done:
        mov     dx, msg_end
        call    puts
leave:
        mov     ax, 0x4C00
        int     0x21

; fm_out: AL = register, AH = value.  A real chip wants a moment between the
;   two writes; reads of an unused port are the usual way to spend it.
fm_out:
        push    ax
        push    cx
        push    dx
        mov     dx, FM_ADDR
        out     dx, al
        mov     cx, 6
.a:     in      al, 0x80
        loop    .a
        mov     dx, FM_DATA
        mov     al, ah
        out     dx, al
        mov     cx, 35
.b:     in      al, 0x80
        loop    .b
        pop     dx
        pop     cx
        pop     ax
        ret

wait_ticks:
        push    ax
        push    bx
        push    cx
        push    dx
        push    es
        xor     ax, ax
        mov     es, ax
        mov     bx, [es:0x046C]
.spin:  mov     ax, [es:0x046C]
        sub     ax, bx
        cmp     ax, cx
        jb      .spin
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

puts:   push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

putc:   push    ax
        push    dx
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     dx
        pop     ax
        ret

crlf:   mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        ret

put_hex8:
        push    ax
        push    cx
        mov     ch, al
        mov     cl, 2
.d:     rol     ch, 4
        mov     al, ch
        and     al, 0x0F
        cmp     al, 10
        jb      .n
        add     al, 7
.n:     add     al, '0'
        call    putc
        dec     cl
        jnz     .d
        pop     cx
        pop     ax
        ret

; =============================================================================
; One two-operator voice on channel 0: a bright attack that decays, which is
; what a game's lead instrument tends to sound like.  Register, then value.
patch:
        db 0x01, 0x20                   ; the waveform registers, switched on
        db 0x20, 0x01                   ; modulator: multiplier one
        db 0x23, 0x01                   ; carrier: the same
        db 0x40, 0x18                   ; modulator held back a little
        db 0x43, 0x00                   ; carrier at full
        db 0x60, 0xF0                   ; both: quick attack, slow decay
        db 0x63, 0xF0
        db 0x80, 0x77                   ; and a middling sustain and release
        db 0x83, 0x77
        db 0xC0, 0x0E                   ; feedback seven, the two in series
        db 0x00, 0x00                   ; the end of the list

; fnum in the low ten bits, block in the next three: A, C, E, A an octave up
notes:  dw 0x1244                       ; block 4, fnum 580   about 440 Hz
        dw 0x12B1                       ; block 4, fnum 689   about 523 Hz
        dw 0x1365                       ; block 4, fnum 869   about 659 Hz
        dw 0x1644                       ; block 5, fnum 580   about 880 Hz
        dw 0

msg_title:   db "ADLIB - four notes through a synthesiser that is not there", 13, 10, "$"
msg_status:  db "  the timer status read $"
msg_then:    db "h, then $"
msg_found:   db "  which is a chip answering.  Setting up one voice.", 13, 10, "$"
msg_none:    db "  which is nobody home: no synthesiser, and no monitor.", 13, 10, "$"
msg_playing: db "  playing four notes", 13, 10, "$"
msg_end:     db "  done.", 13, 10, "$"
