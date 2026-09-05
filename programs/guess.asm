; GUESS.COM - number guessing game.  Exercises DOS buffered input (INT 21h 0Ah)
[BITS 16]
[ORG 0x0100]

start:
        mov     dx, msg_intro
        call    print
new_game:
        ; secret = (BIOS tick count mod 100) + 1
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0x046C]
        xor     dx, dx
        mov     cx, 100
        div     cx
        inc     dx
        mov     [secret], dx
        mov     word [tries], 0
.ask:
        mov     dx, msg_prompt
        call    print
        mov     ah, 0x0A
        mov     dx, inbuf
        int     0x21
        mov     dx, msg_crlf
        call    print
        ; Q quits
        mov     al, [inbuf+2]
        cmp     al, 'q'
        je      .quit
        cmp     al, 'Q'
        je      .quit
        inc     word [tries]
        call    parse_number            ; AX = value, CF = invalid
        jc      .invalid
        cmp     ax, [secret]
        je      .win
        jb      .low
        mov     dx, msg_high
        call    print
        jmp     .ask
.low:   mov     dx, msg_low
        call    print
        jmp     .ask
.invalid:
        mov     dx, msg_invalid
        call    print
        jmp     .ask
.win:
        mov     dx, msg_win
        call    print
        mov     ax, [tries]
        call    print_dec
        mov     dx, msg_win2
        call    print
        mov     dx, msg_again
        call    print
        mov     ah, 0x01
        int     0x21
        push    ax
        mov     dx, msg_crlf
        call    print
        pop     ax
        cmp     al, 'y'
        je      new_game
        cmp     al, 'Y'
        je      new_game
.quit:
        mov     dx, msg_bye
        call    print
        mov     ax, 0x4C00
        int     0x21

; parse_number: decimal digits at inbuf+2 (count at inbuf+1) -> AX. CF=1 if bad
parse_number:
        movzx   cx, byte [inbuf+1]
        jcxz    .bad
        mov     si, inbuf+2
        xor     ax, ax
.digit: mov     bl, [si]
        inc     si
        cmp     bl, ' '
        je      .skip
        sub     bl, '0'
        jb      .bad
        cmp     bl, 9
        ja      .bad
        mov     dx, 10
        mul     dx
        movzx   bx, bl
        add     ax, bx
        cmp     ax, 1000
        ja      .bad
.skip:  loop    .digit
        or      ax, ax
        jz      .bad
        clc
        ret
.bad:   stc
        ret

; print_dec: AX -> decimal digits on screen
print_dec:
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
.div:   xor     dx, dx
        div     bx
        push    dx
        inc     cx
        or      ax, ax
        jnz     .div
.out:   pop     dx
        add     dl, '0'
        mov     ah, 0x02
        int     0x21
        loop    .out
        pop     dx
        pop     cx
        pop     bx
        ret

print:  mov     ah, 0x09
        int     0x21
        ret

msg_intro:  db "GUESS - I am thinking of a number from 1 to 100.", 13, 10
            db "Type your guess and press Enter (Q to quit).", 13, 10, "$"
msg_prompt: db 13, 10, "Your guess? $"
msg_crlf:   db 13, 10, "$"
msg_high:   db "Too high!$"
msg_low:    db "Too low!$"
msg_invalid: db "Please type a number from 1 to 100.$"
msg_win:    db "Correct!  You got it in $"
msg_win2:   db " tries.", 13, 10, "$"
msg_again:  db "Play again (Y/N)? $"
msg_bye:    db "Thanks for playing!", 13, 10, "$"

secret:     dw 0
tries:      dw 0
inbuf:      db 8, 0
            times 10 db 0
