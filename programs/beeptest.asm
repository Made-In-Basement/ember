; =============================================================================
;  BEEPTEST.COM - make sound the way a 1980s DOS game does
; -----------------------------------------------------------------------------
;  Programs timer channel 2 and opens the speaker gate directly, exactly as
;  Alley Cat and its contemporaries do.  Plays a short rising phrase so the
;  speaker bridge (SPEAKER ON) can be checked with a real program rather than
;  a shell command.
; =============================================================================

[BITS 16]
[ORG 0x0100]

start:
        mov     si, msg
        call    puts
        mov     bx, notes
        mov     byte [note_no], '1'
.next:  mov     ax, [bx]
        or      ax, ax
        jz      .done
        mov     al, [note_no]                   ; the note number
        call    putc
        mov     ax, [bx]
        call    tone
        mov     al, 'T'                         ; timer programmed
        call    putc
        mov     cx, 5                           ; about a quarter second
        call    wait_ticks
        mov     al, 'w'                         ; the wait finished
        call    putc
        call    tone_off
        mov     al, 'o'                         ; the tone is off again
        call    putc
        mov     cx, 1
        call    wait_ticks
        mov     al, ' '
        call    putc
        inc     byte [note_no]
        add     bx, 2
        jmp     .next
.done:  call    tone_off
        mov     si, msg_end
        call    puts
        mov     si, msg_done
        call    puts
        mov     si, msg_exit
        call    puts
        mov     ax, 0x4C00
        int     0x21

; tone: AX = frequency in Hz
tone:
        push    ax
        push    bx
        push    dx
        mov     bx, ax
        mov     al, 0xB6                        ; channel 2, square wave
        out     0x43, al
        mov     dx, 0x0012                      ; DX:AX = 1193182
        mov     ax, 0x34DC
        div     bx
        out     0x42, al                        ; divisor, low byte
        mov     al, ah
        out     0x42, al                        ; divisor, high byte
        in      al, 0x61
        or      al, 0x03                        ; gate and speaker on
        out     0x61, al
        pop     dx
        pop     bx
        pop     ax
        ret

tone_off:
        push    ax
        in      al, 0x61
        and     al, 0xFC
        out     0x61, al
        pop     ax
        ret

; wait_ticks: CX = BIOS timer ticks (about 55 ms each)
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

putc:
        push    ax
        push    bx
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        pop     bx
        pop     ax
        ret

puts:
        push    ax
        push    si
.ch:    lodsb
        or      al, al
        jz      .end
        mov     ah, 0x0E
        int     0x10
        jmp     .ch
.end:   pop     si
        pop     ax
        ret

msg:      db "BEEPTEST: playing through timer channel 2, as a 1984 game would.",
          db 13, 10, 0
msg_end:  db 13, 10, "all notes played", 13, 10, 0
msg_done: db "Done.  With SPEAKER ON this comes out of the sound chip.", 13, 10, 0
msg_exit: db "about to exit", 13, 10, 0
note_no:  db '1'
notes:    dw 262, 330, 392, 523, 392, 330, 262, 0
