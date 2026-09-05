; =============================================================================
;  VMODES.COM - what screen modes does this card actually offer?
; -----------------------------------------------------------------------------
;  The boot splash and the graphical shell both have to pick a mode, and
;  picking a 4:3 one on a widescreen panel is what makes a picture look
;  stretched.  Rather than guess what a machine offers, ask it.
;
;  Each line is a mode number, its size, how many bits a pixel takes, and
;  whether it can be reached through the 64 KB window at A000 (which the
;  16-bit kernel needs) or only as one flat region (which the 32-bit
;  desktop can use).  The list is written to C:\VMODES.TXT as well.
; =============================================================================

[BITS 16]
[ORG 0x0100]

MAX_MODES   equ 200

start:
        mov     di, bss_start                   ; a .COM starts with whatever
        mov     cx, bss_end - bss_start         ;  was in memory before it
        xor     al, al
        cld
        rep     stosb

        mov     si, msg_head
        call    puts

        ; ---- the card's own description of itself ----
        push    ds
        pop     es
        mov     di, info
        mov     dword [di], "VBE2"
        mov     ax, 0x4F00
        int     0x10
        cmp     ax, 0x004F
        jne     .no_vbe
        cmp     dword [info], "VESA"
        jne     .no_vbe

        ; ---- copy the mode list out before anything overwrites it ----
        mov     ax, [info+16]
        mov     fs, ax
        mov     si, [info+14]
        mov     di, modes
        xor     cx, cx
.copy:  mov     ax, [fs:si]
        cmp     ax, 0xFFFF
        je      .copied
        mov     [di], ax
        add     si, 2
        add     di, 2
        inc     cx
        cmp     cx, MAX_MODES
        jb      .copy
.copied:
        mov     [count], cx

        mov     si, msg_count
        call    puts
        mov     ax, [count]
        call    print_dec
        call    crlf
        call    crlf

        ; ---- and ask about each one ----
        xor     bp, bp
.next:  cmp     bp, [count]
        jae     .done
        mov     si, bp
        shl     si, 1
        mov     cx, [modes+si]
        push    bp
        push    cx
        mov     ax, 0x4F01
        mov     di, info
        int     0x10
        pop     cx
        pop     bp
        cmp     ax, 0x004F
        jne     .skip
        test    byte [info], 0x01               ; supported at all?
        jz      .skip
        mov     ax, [info+18]
        or      ax, ax
        jz      .skip                           ; not a graphics mode
        cmp     ax, 320
        jb      .skip
        call    show_mode
.skip:  inc     bp
        jmp     .next

.done:  call    write_log
        mov     ax, 0x4C00
        int     0x21
.no_vbe:
        mov     si, msg_novbe
        call    puts
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------- one mode
; CX = mode number, info = its description
show_mode:
        pushad
        mov     ax, cx
        call    print_hex16
        mov     si, msg_sp
        call    puts
        mov     ax, [info+18]                   ; width
        call    print_dec4
        mov     al, 'x'
        call    putc
        mov     ax, [info+20]                   ; height
        call    print_dec4
        mov     si, msg_sp
        call    puts
        movzx   ax, byte [info+25]              ; bits per pixel
        call    print_dec
        mov     si, msg_bpp
        call    puts
        cmp     word [info+8], 0xA000           ; window A at A000?
        jne     .no_window
        mov     si, msg_window
        call    puts
        jmp     .lfb
.no_window:
        mov     si, msg_nowindow
        call    puts
.lfb:   test    byte [info], 0x80               ; a flat region as well?
        jz      .shape
        mov     si, msg_lfb
        call    puts
.shape: ; widescreen or not: width * 100 / height
        mov     ax, [info+18]
        mov     bx, [info+20]
        or      bx, bx
        jz      .end
        xor     dx, dx
        mov     cx, 100
        mul     cx
        div     bx
        cmp     ax, 155
        jb      .end
        cmp     ax, 185
        ja      .end
        mov     si, msg_wide
        call    puts
.end:   call    crlf
        call    page_break
        popad
        ret

; ---------------------------------------------------------------- the file
write_log:
        mov     dx, log_name
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .failed
        mov     bx, ax
        mov     cx, [log_len]
        mov     dx, log_buf
        mov     ah, 0x40
        int     0x21
        mov     ah, 0x3E
        int     0x21
        mov     si, msg_saved
        call    puts_screen
        ret
.failed:
        mov     si, msg_nosave
        call    puts_screen
        ret

; ---------------------------------------------------------------- screen
page_break:
        inc     word [lines]
        cmp     word [lines], 21
        jb      .done
        mov     word [lines], 0
        push    si
        mov     si, msg_more
        call    puts_screen
        pop     si
        xor     ah, ah
        int     0x16
        call    crlf
.done:  ret

puts:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:  pop     ax
        ret

puts_screen:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc_screen
        jmp     .loop
.done:  pop     ax
        ret

putc:
        call    putc_screen
        push    bx
        mov     bx, [log_len]
        cmp     bx, LOG_MAX - 1
        jae     .full
        mov     [log_buf+bx], al
        inc     word [log_len]
.full:  pop     bx
        ret

putc_screen:
        push    ax
        push    bx
        mov     ah, 0x0E
        xor     bx, bx
        int     0x10
        pop     bx
        pop     ax
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

; AX in four columns, space padded
print_dec4:
        push    ax
        push    bx
        mov     bx, ax
        cmp     bx, 1000
        jae     .go
        mov     al, ' '
        call    putc
        cmp     bx, 100
        jae     .go
        mov     al, ' '
        call    putc
        cmp     bx, 10
        jae     .go
        mov     al, ' '
        call    putc
.go:    mov     ax, bx
        call    print_dec
        pop     bx
        pop     ax
        ret

print_dec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
        or      ax, ax
        jnz     .split
        mov     al, '0'
        call    putc
        jmp     .out
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
.out:   pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

print_hex16:
        push    ax
        mov     al, ah
        call    print_hex8
        pop     ax
        call    print_hex8
        ret

print_hex8:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .digit
        mov     al, cl
        and     al, 0x0F
        call    .digit
        pop     cx
        pop     ax
        ret
.digit: and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    putc
        ret

msg_head:     db "Screen modes this card offers", 13, 10
              db "mode  size       depth  how it is reached", 13, 10
              db "------------------------------------------------", 13, 10, 0
msg_count:    db "modes listed: ", 0
msg_sp:       db "  ", 0
msg_bpp:      db " bpp  ", 0
msg_window:   db "window", 0
msg_nowindow: db "  --  ", 0
msg_lfb:      db " +flat", 0
msg_wide:     db "  <- widescreen", 0
msg_more:     db "-- press a key --", 13, 10, 0
msg_novbe:    db "This card does not answer VESA calls at all.", 13, 10, 0
msg_saved:    db "Written to C:\VMODES.TXT", 13, 10, 0
msg_nosave:   db "Could not write C:\VMODES.TXT", 13, 10, 0
log_name:     db "\VMODES.TXT", 0

LOG_MAX     equ 12288

section .bss
bss_start:
info:       resb 512
modes:      resw MAX_MODES
count:      resw 1
lines:      resw 1
log_len:    resw 1
log_buf:    resb LOG_MAX
bss_end:
