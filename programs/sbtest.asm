; =============================================================================
;  SBTEST.COM - is there a sound card, and is it the one the host invented?
; -----------------------------------------------------------------------------
;  Enters the DPMI host as a client, then goes looking for a Sound Blaster
;  exactly as a game's setup program would: reset the digital processor and
;  wait for AAh, ask its version, then write a note to the synthesiser and
;  read its timers back.
;
;  There is no such card in this machine.  Every one of those ports is
;  refused to the client by the task state segment's permission map, the
;  processor faults, and the host answers in the card's place.  A program
;  cannot tell the difference from outside, which is the entire point.
; =============================================================================

[BITS 16]
[ORG 0x0100]

start:
        mov     [rm_seg], cs
        mov     si, msg_head
        call    puts_rm

        ; ---- a host to run under ----
        mov     ax, 0x1687
        int     0x2F
        or      ax, ax
        jz      .have_host
        mov     si, msg_no_host
        call    puts_rm
        jmp     bye
.have_host:
        mov     [entry], di
        mov     [entry+2], es
        mov     [host_paras], si
        test    bl, 1
        jnz     .can32
        mov     si, msg_no32
        call    puts_rm
        jmp     bye
.can32:
        mov     ax, cs
        mov     es, ax
        mov     bx, 0x1000                      ; hand the rest of memory back
        mov     ah, 0x4A
        int     0x21
        mov     bx, 0x0100                      ; 4 KB for samples to come from
        mov     ah, 0x48
        int     0x21
        jc      .no_buf
        mov     [buf_seg], ax
        ; A square wave, so that whatever comes out of the speakers is
        ; unmistakably ours and not a click.  Twenty samples high and twenty
        ; low at about 11 kHz is a note near 275 Hz.
        push    es
        mov     es, ax
        xor     di, di
        mov     cx, 4096 / 40
.tone:  mov     al, 0xC0
        push    cx
        mov     cx, 20
        rep     stosb
        mov     al, 0x40
        mov     cx, 20
        rep     stosb
        pop     cx
        loop    .tone
        pop     es
.no_buf:
        mov     bx, [host_paras]
        or      bx, bx
        jz      .no_data
        mov     ah, 0x48
        int     0x21
        jc      .no_data
        mov     es, ax
.no_data:
        mov     ax, 1                           ; a 32-bit client
        call    far [entry]
        jnc     protected
        mov     si, msg_refused
        call    puts_rm
bye:    mov     ax, 0x4C00
        int     0x21

; =============================================================================
;  From here the ports belong to the host
; =============================================================================
protected:
        mov     si, msg_in_pm
        call    puts

        ; ---- 1. the reset handshake: 1, then 0, then AAh comes back ----
        mov     si, msg_reset
        call    puts
        mov     dx, 0x226
        mov     al, 1
        out     dx, al
        xor     cx, cx
.settle:                                        ; the real chip needs a moment
        loop    .settle
        xor     al, al
        out     dx, al
        mov     cx, 200
.wait:  mov     dx, 0x22E
        in      al, dx
        test    al, 0x80
        jnz     .ready
        loop    .wait
        mov     si, msg_no_answer
        call    puts
        jmp     leave
.ready: mov     dx, 0x22A
        in      al, dx
        mov     [got_reset], al
        call    put_hex8
        cmp     al, 0xAA
        je      .reset_ok
        mov     si, msg_not_aa
        call    puts
        jmp     .version
.reset_ok:
        mov     si, msg_is_aa
        call    puts

        ; ---- 2. which version? ----
.version:
        mov     si, msg_version
        call    puts
        mov     dx, 0x22C
        mov     al, 0xE1
        out     dx, al
        call    dsp_get
        mov     [ver_major], al
        call    put_dec
        mov     al, '.'
        call    putc
        call    dsp_get
        mov     [ver_minor], al
        call    put_dec
        call    crlf

        ; ---- 3. the synthesiser: write, then read the timers ----
        mov     si, msg_fm
        call    puts
        mov     dx, 0x388                       ; register 1: waveform select
        mov     al, 0x01
        out     dx, al
        mov     dx, 0x389
        mov     al, 0x20
        out     dx, al
        mov     dx, 0x388                       ; a note on channel 0
        mov     al, 0xA0
        out     dx, al
        mov     dx, 0x389
        mov     al, 0x98
        out     dx, al
        mov     dx, 0x388
        in      al, dx
        call    put_hex8
        mov     si, msg_fm_tail
        call    puts

        ; ---- 4. programme a transfer, the way a game does ----
        cmp     word [buf_seg], 0
        je      .no_transfer
        mov     si, msg_dma
        call    puts

        mov     dx, 0x22C                       ; the speaker on
        mov     al, 0xD1
        out     dx, al
        mov     al, 0x40                        ; and the rate: 11 kHz
        out     dx, al
        mov     al, 0xA5
        out     dx, al

        mov     dx, 0x0A                        ; hold channel 1 still
        mov     al, 0x05
        out     dx, al
        mov     dx, 0x0C                        ; and start its halves afresh
        xor     al, al
        out     dx, al
        mov     dx, 0x0B                        ; read from memory, one pass
        mov     al, 0x49
        out     dx, al

        movzx   eax, word [buf_seg]             ; the buffer, as the bus sees it
        shl     eax, 4
        mov     [buf_phys], eax
        mov     dx, 0x02
        out     dx, al                          ; address, low half
        mov     al, ah
        out     dx, al                          ; and high
        shr     eax, 16
        mov     dx, 0x83                        ; the page above them
        out     dx, al

        mov     dx, 0x03                        ; how many bytes, less one
        mov     al, 0xFF
        out     dx, al
        mov     al, 0x0F
        out     dx, al

        mov     dx, 0x0A                        ; let it go
        mov     al, 0x01
        out     dx, al

        mov     dx, 0x22C                       ; the block the DSP will play
        mov     al, 0x48
        out     dx, al
        mov     al, 0xFF
        out     dx, al
        mov     al, 0x0F
        out     dx, al
        mov     al, 0x1C                        ; and keep playing it
        out     dx, al

        ; Give it time to be heard.  The host feeds the stream on the timer
        ; interrupt, so a client that starts a sound and leaves at once has
        ; started nothing anybody could hear.
        mov     si, msg_waiting
        call    puts
        mov     ecx, 0x04000000
.hold:  dec     ecx
        jnz     .hold

        mov     si, msg_dma_at
        call    puts
        mov     eax, [buf_phys]
        shr     eax, 16
        call    put_hex8
        mov     eax, [buf_phys]
        shr     eax, 8
        call    put_hex8
        mov     eax, [buf_phys]
        call    put_hex8
        call    crlf
        jmp     leave
.no_transfer:
        mov     si, msg_no_buf
        call    puts

leave:
        mov     si, msg_bye
        call    puts
        mov     ax, 0x4C00
        int     0x21

; a byte from the processor, once it says there is one
dsp_get:
        push    cx
        push    dx
        mov     cx, 400
.poll:  mov     dx, 0x22E
        in      al, dx
        test    al, 0x80
        jnz     .got
        loop    .poll
        mov     al, 0xFF
        pop     dx
        pop     cx
        ret
.got:   mov     dx, 0x22A
        in      al, dx
        pop     dx
        pop     cx
        ret

; ---------------------------------------------------------------- printing
; Before the switch the BIOS will do.  Afterwards it will not: a client has
; no protected-mode handler for INT 10h, and issuing one is simply a fault.
; The host runs real-mode interrupts on request instead, which is what
; function 0300h is for.
puts_rm:
        push    ax
        push    bx
.loop:  lodsb
        or      al, al
        jz      .done
        mov     ah, 0x0E
        xor     bx, bx
        int     0x10
        jmp     .loop
.done:  pop     bx
        pop     ax
        ret

puts:
        pushad
        push    es
        mov     di, out_buf
.copy:  lodsb
        or      al, al
        jz      .end
        mov     [di], al
        inc     di
        cmp     di, out_buf + 250
        jb      .copy
.end:   mov     byte [di], '$'
        push    ds
        pop     es
        ; The host is handed this at ES:EDI, all thirty-two bits of it.  Set
        ; only DI and the top half is whatever was left there, which sends
        ; the host's answer somewhere else entirely - over the descriptor
        ; table, if the day is going badly.
        mov     edi, rmcs
        mov     cx, 25
        xor     ax, ax
        cld
        rep     stosw
        mov     edi, rmcs                       ; stosw walked DI to the end of it
        mov     ax, [rm_seg]
        mov     [rmcs+36], ax                   ; DS
        mov     dword [rmcs+20], out_buf        ; EDX
        mov     dword [rmcs+28], 0x00000900     ; EAX: print the string
        mov     ax, 0x0300
        mov     bl, 0x21
        xor     bh, bh
        xor     cx, cx
        int     0x31
        pop     es
        popad
        ret

putc:
        push    si
        mov     [char_buf], al
        mov     byte [char_buf+1], 0
        mov     si, char_buf
        call    puts
        pop     si
        ret

crlf:   push    si
        mov     si, msg_crlf
        call    puts
        pop     si
        ret

put_hex8:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .nib
        mov     al, cl
        and     al, 0x0F
        call    .nib
        pop     cx
        pop     ax
        ret
.nib:   cmp     al, 10
        jb      .digit
        add     al, 'A' - 10
        jmp     .out
.digit: add     al, '0'
.out:   call    putc
        ret

put_dec:                                        ; AL, no padding
        push    ax
        push    bx
        push    cx
        push    dx
        movzx   ax, al
        xor     cx, cx
        mov     bx, 10
        or      ax, ax
        jnz     .split
        mov     al, '0'
        call    putc
        jmp     .done
.split: xor     dx, dx
        div     bx
        push    dx
        inc     cx
        or      ax, ax
        jnz     .split
.emit:  pop     ax
        add     al, '0'
        call    putc
        loop    .emit
.done:  pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------- words
msg_head:      db "SBTEST: looking for a sound card that is not there", 13, 10, 13, 10, 0
msg_no_host:   db "No DPMI host.  LOAD DPMI first.", 13, 10, 0
msg_no32:      db "The host does not take 32-bit clients.", 13, 10, 0
msg_refused:   db "The host refused the client.", 13, 10, 0
msg_in_pm:     db "In protected mode; the card's ports are the host's now.", 13, 10, 13, 10, 0
msg_reset:     db "  reset answered with     ", 0
msg_is_aa:     db "  (AAh: a card is there)", 13, 10, 0
msg_not_aa:    db "  (expected AAh)", 13, 10, 0
msg_no_answer: db "  nothing answered the reset", 13, 10, 0
msg_version:   db "  the version it claims   ", 0
msg_fm:        db "  the synthesiser's timers ", 0
msg_fm_tail:   db "  (C0h once both have run)", 13, 10, 0
msg_dma:       db "  a transfer, programmed as a game would", 13, 10, 0
msg_waiting:   db "  playing, for a moment", 13, 10, 0
msg_dma_at:    db "  the buffer it was given ", 0
msg_no_buf:    db "  no memory for a buffer", 13, 10, 0
msg_bye:       db 13, 10, "Leaving; EMBER.LOG has what the card was told.", 13, 10, 0

msg_crlf:      db 13, 10, 0

rm_seg:        dw 0
entry:         dd 0
host_paras:    dw 0
got_reset:     db 0
ver_major:     db 0
ver_minor:     db 0
buf_seg:       dw 0
buf_phys:      dd 0
char_buf:      times 4 db 0
rmcs:          times 50 db 0
out_buf:       times 256 db 0
