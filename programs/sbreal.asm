; =============================================================================
;  SBREAL.COM - an ordinary DOS program looking for a Sound Blaster
; -----------------------------------------------------------------------------
;  SBTEST.COM does this from inside a DPMI client, which is a thing almost no
;  game is.  This one is a plain .COM file in real mode, doing exactly what a
;  game's setup does and then what its sound code does: find the card, program
;  the transfer controller, tell the DSP to play, and wait to be told the
;  block has been played.
;
;  On a machine with no card and no monitor it finds nothing and says so.
;  With SB.MOD loaded there is no card either, but the program is running in
;  virtual-8086 mode and every port it touches is answered by the monitor, so
;  it finds one.
;
;  The block plays over and over (the DSP's auto-initialise command), so the
;  card's own interrupt should arrive several times a second.  A game that
;  never gets it hangs waiting; the count printed at the end is the thing
;  worth reading.
; =============================================================================

[BITS 16]
[ORG 0x100]

SB_BASE         equ 0x220
DSP_RESET       equ SB_BASE + 6
DSP_READ        equ SB_BASE + 0x0A
DSP_WRITE       equ SB_BASE + 0x0C
DSP_STATUS      equ SB_BASE + 0x0E

RATE            equ 11025
TIME_CONST      equ 256 - (1000000 / RATE)      ; what the DSP is told instead
TONE            equ 440
BUF_BYTES       equ 2048
CARD_IRQ        equ 5
DMA_CHAN        equ 1                           ; the card's transfer channel
CARD_VECTOR     equ 8 + CARD_IRQ

start:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld
        mov     dx, msg_title
        call    puts

; ---- the reset handshake: a card answers AAh -------------------------------
        call    dsp_reset
        jnc     .found
        mov     dx, msg_none
        call    puts
        jmp     leave_quietly
.found:
        mov     dx, msg_found
        call    puts

; ---- what version it says it is --------------------------------------------
        mov     al, 0xE1
        call    dsp_write
        call    dsp_read
        mov     bh, al
        call    dsp_read
        mov     bl, al
        mov     dx, msg_version
        call    puts
        mov     al, bh
        call    put_dec
        mov     al, '.'
        call    putc
        mov     al, bl
        call    put_dec
        call    crlf
        ; SBREAL /Q: stop here, having only asked whether a card is there.
        ; The rest of this - a buffer, a transfer, a tone - is what a game
        ; does next, and being able to stop before it is how the two halves
        ; are told apart when something goes wrong.
        cmp     byte [0x80], 0
        jne     leave_quietly

; ---- somewhere for the samples that a transfer can reach --------------------
;  The controller carries a sixteen-bit address and a page above it, so a
;  block must not cross a 64 KB boundary.  Twice the room is asked for and the
;  aligned half of it used, which cannot.
        call    shrink                          ; give the rest of memory back
        mov     bx, (BUF_BYTES * 2) / 16 + 16
        mov     ah, 0x48
        int     0x21
        jnc     .got
        mov     dx, msg_no_mem
        call    puts
        jmp     leave_quietly
.got:   mov     [buf_seg], ax
        movzx   eax, ax
        shl     eax, 4
        add     eax, BUF_BYTES - 1
        and     eax, ~(BUF_BYTES - 1)           ; aligned, so it cannot cross
        mov     [buf_phys], eax
        shr     eax, 4
        mov     [buf_seg], ax
        mov     dx, msg_buffer
        call    puts
        mov     eax, [buf_phys]
        call    put_hex32
        call    crlf

; ---- a square wave, as a game's sound effect would be ----------------------
        push    es
        mov     es, [buf_seg]
        xor     di, di
        mov     cx, BUF_BYTES
        mov     bx, RATE / (TONE * 2)           ; samples in half a cycle
        mov     dx, bx
        mov     al, 0xC0
.fill:  stosb
        dec     dx
        jnz     .same
        mov     dx, bx
        xor     al, 0xC0 ^ 0x40                 ; the other half of the wave
.same:  loop    .fill
        pop     es

; ---- the card's own interrupt, which is what a game waits for --------------
        push    es
        xor     ax, ax
        mov     es, ax
        cli
        mov     ax, [es:CARD_VECTOR*4]
        mov     [old_vec], ax
        mov     ax, [es:CARD_VECTOR*4+2]
        mov     [old_vec+2], ax
        mov     word [es:CARD_VECTOR*4], card_isr
        mov     [es:CARD_VECTOR*4+2], cs
        in      al, 0x21
        mov     [old_mask], al
        and     al, ~(1 << CARD_IRQ)            ; let it through
        out     0x21, al
        sti
        pop     es

; ---- the transfer, programmed the way a game programs it -------------------
        mov     al, 0x04 | DMA_CHAN             ; hold the channel still
        out     0x0A, al
        xor     al, al                          ; and start its halves afresh
        out     0x0C, al
        mov     al, 0x58 | DMA_CHAN             ; read from memory, over again
        out     0x0B, al
        mov     eax, [buf_phys]
        out     0x02, al                        ; the address, low half
        mov     al, ah
        out     0x02, al                        ; and high
        shr     eax, 16
        out     0x83, al                        ; the page above them
        mov     ax, BUF_BYTES - 1
        out     0x03, al                        ; how many bytes, less one
        mov     al, ah
        out     0x03, al
        mov     al, DMA_CHAN                    ; let it go
        out     0x0A, al

; ---- and the DSP told to play it -------------------------------------------
        mov     al, 0xD1                        ; the speaker on
        call    dsp_write
        mov     al, 0x40                        ; how fast
        call    dsp_write
        mov     al, TIME_CONST
        call    dsp_write
        mov     al, 0x48                        ; how long a block is
        call    dsp_write
        mov     ax, BUF_BYTES - 1
        call    dsp_write
        mov     al, ah
        call    dsp_write
        mov     al, 0x1C                        ; play it, and keep playing
        call    dsp_write

        mov     dx, msg_playing
        call    puts
        mov     cx, 54                          ; about three seconds of it
        call    wait_ticks

; ---- stop, and put back what was borrowed ----------------------------------
        mov     al, 0xD0                        ; hold
        call    dsp_write
        mov     al, 0xD3                        ; the speaker off
        call    dsp_write
        call    dsp_reset
        mov     al, 0x04 | DMA_CHAN             ; the channel still again
        out     0x0A, al

        mov     dx, msg_irqs
        call    puts
        mov     ax, [irq_count]
        call    put_dec16
        call    crlf
        cmp     word [irq_count], 0
        jne     .heard
        mov     dx, msg_no_irq
        call    puts
        jmp     .restore
.heard: mov     dx, msg_ok
        call    puts
.restore:
        push    es
        xor     ax, ax
        mov     es, ax
        cli
        mov     ax, [old_vec]
        mov     [es:CARD_VECTOR*4], ax
        mov     ax, [old_vec+2]
        mov     [es:CARD_VECTOR*4+2], ax
        mov     al, [old_mask]
        out     0x21, al
        sti
        pop     es
leave_quietly:
        mov     ax, 0x4C00
        int     0x21

; =============================================================================
; card_isr: the card says a block has been played.  A game refills the buffer
;   here; this only counts, tells the card it has been heard, and tells the
;   controller the same.
; =============================================================================
card_isr:
        push    ax
        push    dx
        inc     word [cs:irq_count]
        mov     dx, DSP_STATUS                  ; the card, acknowledged
        in      al, dx
        mov     al, 0x20                        ; and the controller
        out     0x20, al
        pop     dx
        pop     ax
        iret

; =============================================================================
;  Talking to the DSP
; =============================================================================
; dsp_reset: 1 into the reset port, a pause, 0, then AAh should be readable.
;   CF=1 if nothing answers.
dsp_reset:
        mov     dx, DSP_RESET
        mov     al, 1
        out     dx, al
        mov     cx, 100
.pause: in      al, 0x80
        loop    .pause
        xor     al, al
        out     dx, al
        mov     cx, 4000
.wait:  mov     dx, DSP_STATUS
        in      al, dx
        test    al, 0x80
        jnz     .ready
        loop    .wait
        stc
        ret
.ready: mov     dx, DSP_READ
        in      al, dx
        cmp     al, 0xAA
        je      .yes
        stc
        ret
.yes:   clc
        ret

; dsp_write: AL to the card, when it says it is ready
dsp_write:
        push    ax
        push    cx
        push    dx
        mov     cx, 0xFFFF
        mov     dx, DSP_WRITE
.wait:  in      al, dx
        test    al, 0x80
        jz      .ready
        loop    .wait
.ready: pop     dx
        pop     cx
        pop     ax
        push    dx
        mov     dx, DSP_WRITE
        out     dx, al
        pop     dx
        ret

; dsp_read: -> AL
dsp_read:
        push    cx
        push    dx
        mov     cx, 0xFFFF
.wait:  mov     dx, DSP_STATUS
        in      al, dx
        test    al, 0x80
        jnz     .ready
        loop    .wait
        mov     al, 0xFF
        pop     dx
        pop     cx
        ret
.ready: mov     dx, DSP_READ
        in      al, dx
        pop     dx
        pop     cx
        ret

; =============================================================================
;  Odds and ends
; =============================================================================
; shrink: AH = 4Ah with BX paragraphs; the block is the program's own
shrink:
        push    ax
        push    bx
        mov     ax, cs
        mov     es, ax
        mov     bx, 0x100                       ; 4 KB: the program and a stack
        mov     ah, 0x4A
        int     0x21
        push    cs
        pop     es
        pop     bx
        pop     ax
        ret

; wait_ticks: CX ticks of 55 ms, by the count the BIOS keeps
wait_ticks:
        push    cx
        xor     ah, ah
        int     0x1A
        mov     [tick0], dx
        pop     cx
.again: xor     ah, ah
        int     0x1A
        sub     dx, [tick0]
        cmp     dx, cx
        jb      .again
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

; put_dec: AL, in decimal, without leading zeros
put_dec:
        push    ax
        movzx   ax, al
        call    put_dec16
        pop     ax
        ret

put_dec16:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
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
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; put_hex32: EAX in hex
put_hex32:
        push    ecx
        mov     ecx, 8
.digit: rol     eax, 4
        push    eax
        and     al, 0x0F
        cmp     al, 10
        jb      .num
        add     al, 'A' - 10 - '0'
.num:   add     al, '0'
        call    putc
        pop     eax
        loop    .digit
        pop     ecx
        ret

; =============================================================================
msg_title:      db "SBREAL - a DOS program asking whether there is a card", 13, 10, "$"
msg_none:       db "  nothing answered the reset: no card, and no monitor", 13, 10, "$"
msg_found:      db "  the reset answered AAh: something is there", 13, 10, "$"
msg_version:    db "  it says it is a DSP version $"
msg_buffer:     db "  the samples go at $"
msg_no_mem:     db "  no room for a buffer", 13, 10, "$"
msg_playing:    db "  playing a tone, three seconds of it", 13, 10, "$"
msg_irqs:       db "  the card interrupted this many times: $"
msg_no_irq:     db "  ...which means a game would have hung here", 13, 10, "$"
msg_ok:         db "  a game would have kept its buffer full", 13, 10, "$"

buf_seg:        dw 0
buf_phys:       dd 0
old_vec:        dd 0
old_mask:       db 0
                align 2
irq_count:      dw 0
tick0:          dw 0
