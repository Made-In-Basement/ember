; =============================================================================
;  console.asm - text console I/O on top of the BIOS (INT 10h / INT 16h)
; =============================================================================

; -----------------------------------------------------------------------------
; putc: print AL (teletype; handles CR, LF, BS, BEL)
; -----------------------------------------------------------------------------
putc:
        push    ax
        push    bx
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        pop     bx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; puts: print NUL-terminated string at DS:SI
; -----------------------------------------------------------------------------
puts:
        push    ax
        push    si
.next:  lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .next
.done:  pop     si
        pop     ax
        ret

; -----------------------------------------------------------------------------
; puts_n: print CX bytes at DS:SI (raw)
; -----------------------------------------------------------------------------
puts_n:
        push    ax
        push    cx
        push    si
        jcxz    .done
.next:  lodsb
        call    putc
        loop    .next
.done:  pop     si
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; crlf: print CR LF
; -----------------------------------------------------------------------------
crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

; -----------------------------------------------------------------------------
; put_spaces: print CX spaces
; -----------------------------------------------------------------------------
put_spaces:
        push    ax
        push    cx
        jcxz    .done
        mov     al, ' '
.next:  call    putc
        loop    .next
.done:  pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; print_dec: print EAX as unsigned decimal
; -----------------------------------------------------------------------------
print_dec:
        pushad
        mov     ebx, 10
        xor     cx, cx
.divide:
        xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .divide
.emit:  pop     ax
        add     al, '0'
        call    putc
        loop    .emit
        popad
        ret

; -----------------------------------------------------------------------------
; print_dec_pad: print EAX right-aligned in a field of CL characters
; -----------------------------------------------------------------------------
print_dec_pad:
        pushad
        ; count digits
        push    eax
        xor     ch, ch                  ; CH = digit count
        mov     ebx, 10
.count: xor     edx, edx
        div     ebx
        inc     ch
        test    eax, eax
        jnz     .count
        pop     eax
        sub     cl, ch
        jbe     .print
        push    cx
        movzx   cx, cl
        call    put_spaces
        pop     cx
.print: call    print_dec
        popad
        ret

; -----------------------------------------------------------------------------
; print_2d: print AL (0..99) as two decimal digits, leading zero
; print_2d_sp: same, but a leading space instead of zero
; -----------------------------------------------------------------------------
print_2d:
        push    ax
        aam                             ; AH = AL/10, AL = AL%10
        xchg    al, ah
        add     al, '0'
        call    putc
        mov     al, ah
        add     al, '0'
        call    putc
        pop     ax
        ret

print_2d_sp:
        push    ax
        aam
        xchg    al, ah
        add     al, '0'
        cmp     al, '0'
        jne     .digit
        mov     al, ' '
.digit: call    putc
        mov     al, ah
        add     al, '0'
        call    putc
        pop     ax
        ret

; -----------------------------------------------------------------------------
; print_hex16: print AX as 4 hex digits
; -----------------------------------------------------------------------------
print_hex16:
        push    ax
        push    cx
        mov     cx, 4
.next:  rol     ax, 4
        push    ax
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    putc
        pop     ax
        loop    .next
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; getkey: wait for a key. Returns AL = ASCII, AH = scan code
; kbhit:  ZF=0 if a key is waiting (AX = that key, not removed)
; -----------------------------------------------------------------------------
getkey:
        xor     ah, ah
        int     0x16
        ret

kbhit:
        ; compare the BIOS keyboard buffer head and tail pointers directly;
        ; this avoids INT 16h AH=01h quirks on some real BIOSes
        push    ds
        push    ax
        xor     ax, ax
        mov     ds, ax
        mov     ax, [0x041A]
        cmp     ax, [0x041C]
        pop     ax
        pop     ds
        ret

; -----------------------------------------------------------------------------
; kb_flush: discard any pending keystrokes
; -----------------------------------------------------------------------------
kb_flush:
        push    ax
.again: call    kbhit
        jz      .done
        call    getkey
        jmp     .again
.done:  pop     ax
        ret

; -----------------------------------------------------------------------------
; kb_hw_reset: empty the 8042 output buffer and the BIOS keyboard buffer.
;   A byte left unread in the controller keeps IRQ1 asserted, and because the
;   interrupt is edge triggered no further keystroke would ever be reported.
;   A 32-bit program that polls the controller itself can leave one behind, so
;   this runs when such a program exits.
; -----------------------------------------------------------------------------
kb_hw_reset:
        push    ax
        push    cx
        push    ds
        mov     cx, 64                          ; bounded: never wait forever
.drain: in      al, 0x64
        test    al, 0x01                        ; output buffer full?
        jz      .empty
        in      al, 0x60
        loop    .drain
.empty: xor     ax, ax
        mov     ds, ax
        mov     ax, [0x0480]                    ; buffer start (usually 0x1E)
        mov     [0x041A], ax                    ; head = tail = start: empty
        mov     [0x041C], ax
        and     byte [0x0417], 0xF0             ; drop stuck Shift/Ctrl/Alt
        mov     byte [0x0418], 0
        pop     ds
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; readline: line editor.  DS:DI = buffer, CX = max characters (excl. NUL).
;   Returns AX = length; buffer is NUL-terminated.  Does not print a newline.
;   Backspace edits, ESC clears the line.
; -----------------------------------------------------------------------------
readline:
        push    bx
        push    cx
        push    dx
        xor     bx, bx                  ; BX = current length
        mov     dx, cx                  ; DX = capacity
.loop:
        call    getkey
        cmp     al, 13
        je      .done
        cmp     al, 8
        je      .backspace
        cmp     al, 27
        je      .clear
        cmp     al, 9
        jne     .not_tab
        cmp     word [readline_tab], 0
        je      .loop
        call    word [readline_tab]     ; DS:DI, BX, DX -> BX
        jmp     .loop
.not_tab:
        cmp     al, 32
        jb      .loop                   ; ignore control / extended keys
        cmp     al, 0xE0
        je      .loop
        cmp     bx, dx
        jae     .loop                   ; buffer full
        mov     [di+bx], al
        inc     bx
        call    putc
        jmp     .loop
.backspace:
        test    bx, bx
        jz      .loop
        dec     bx
        call    .erase_one
        jmp     .loop
.clear:
        test    bx, bx
        jz      .loop
        dec     bx
        call    .erase_one
        jmp     .clear
.done:
        mov     byte [di+bx], 0
        mov     ax, bx
        pop     dx
        pop     cx
        pop     bx
        ret
.erase_one:
        mov     al, 8
        call    putc
        mov     al, ' '
        call    putc
        mov     al, 8
        call    putc
        ret

readline_tab:   dw 0                    ; the shell's completion, at the prompt only

; -----------------------------------------------------------------------------
; cls: clear the screen with the current attribute and home the cursor
; -----------------------------------------------------------------------------
cls:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     ax, 0x0600
        mov     bh, [screen_attr]
        xor     cx, cx
        mov     dx, 0x184F
        int     0x10
        mov     ah, 0x02
        xor     bh, bh
        xor     dx, dx
        int     0x10
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; set_text_mode: (re)enter 80x25 colour text mode
; -----------------------------------------------------------------------------
set_text_mode:
        push    ax
        mov     ax, 0x0003
        int     0x10
        pop     ax
        ret

; -----------------------------------------------------------------------------
; upcase: AL -> upper case
; -----------------------------------------------------------------------------
upcase:
        cmp     al, 'a'
        jb      .done
        cmp     al, 'z'
        ja      .done
        sub     al, 32
.done:  ret

; -----------------------------------------------------------------------------
; strcmp: compare NUL-terminated DS:SI with DS:DI.  ZF=1 if equal.
;   Preserves SI/DI.
; -----------------------------------------------------------------------------
strcmp:
        push    ax
        push    si
        push    di
.next:  mov     al, [si]
        cmp     al, [di]
        jne     .done
        or      al, al
        jz      .done
        inc     si
        inc     di
        jmp     .next
.done:  pop     di
        pop     si
        pop     ax
        ret

; -----------------------------------------------------------------------------
; skip_spaces: advance DS:SI past blanks and tabs
; -----------------------------------------------------------------------------
skip_spaces:
        push    ax
.next:  mov     al, [si]
        cmp     al, ' '
        je      .skip
        cmp     al, 9
        jne     .done
.skip:  inc     si
        jmp     .next
.done:  pop     ax
        ret

; -----------------------------------------------------------------------------
; bios_ticks: EAX = BIOS timer tick count (18.2 Hz) from the BIOS data area
; -----------------------------------------------------------------------------
bios_ticks:
        push    ds
        push    ax
        xor     ax, ax
        mov     ds, ax
        pop     ax
        mov     eax, [0x046C]
        pop     ds
        ret

; -----------------------------------------------------------------------------
; delay_ticks: wait CX BIOS ticks (55 ms each)
; -----------------------------------------------------------------------------
delay_ticks:
        pushad
        call    bios_ticks
        mov     ebx, eax                ; EBX = start
        movzx   ecx, cx                 ; ECX = ticks to wait
.wait:  hlt                             ; sleep until the next interrupt
        call    bios_ticks
        sub     eax, ebx                ; elapsed (modular, survives wrap)
        cmp     eax, ecx
        jb      .wait
        popad
        ret
