; =============================================================================
;  XMSTEST.COM - is there extended memory, and does it hold what is put in it?
; -----------------------------------------------------------------------------
;  Asks the way a DOS program does: INT 2Fh AX=4300h for whether a driver is
;  listening, AX=4310h for the address to call, then the version, how much is
;  free, and a block to try.  A pattern is written into that block, read back
;  into a different buffer, and compared, which is the only way to know the
;  copy really goes above the first megabyte and comes back.
; =============================================================================

[BITS 16]
[ORG 0x0100]

start:
        mov     si, msg_head
        call    puts

        ; ---- is anybody there? ----
        mov     ax, 0x4300
        int     0x2F
        cmp     al, 0x80
        je      .present
        mov     si, msg_none
        call    puts
        jmp     .bye
.present:
        mov     si, msg_present
        call    puts

        ; ---- the address to call ----
        mov     ax, 0x4310
        int     0x2F
        mov     [entry], bx
        mov     [entry+2], es
        push    ds                              ; the call handed back its own ES
        pop     es                              ; and the string moves below need ours
        mov     si, msg_entry
        call    puts
        mov     ax, es
        call    put_hex16
        mov     al, ':'
        call    putc
        mov     ax, bx
        call    put_hex16
        call    crlf

        ; ---- which version ----
        mov     ah, 0x00
        call    far [entry]
        mov     si, msg_version
        call    puts
        call    put_hex16
        call    crlf

        ; ---- how much is free ----
        mov     ah, 0x08
        xor     bl, bl
        call    far [entry]
        push    dx
        mov     si, msg_largest
        call    puts
        call    put_dec
        mov     si, msg_kb
        call    puts
        pop     ax
        mov     si, msg_total
        call    puts
        call    put_dec
        mov     si, msg_kb
        call    puts

        ; ---- a block of 64 KB ----
        mov     ah, 0x09
        mov     dx, 64
        call    far [entry]
        or      ax, ax
        jnz     .got
        mov     si, msg_noalloc
        call    puts
        jmp     .bye
.got:   mov     [handle], dx
        mov     si, msg_handle
        call    puts
        mov     ax, dx
        call    put_hex16
        call    crlf

        ; ---- fill a buffer, send it up, bring it back, compare ----
        mov     di, buf_out
        mov     cx, 256
        mov     ax, 0x1234
.fill:  stosw
        add     ax, 0x1111
        loop    .fill

        mov     word [mv_len], 512              ; 512 bytes
        mov     word [mv_len+2], 0
        mov     word [mv_src_h], 0              ; from conventional memory
        mov     word [mv_src_off], buf_out
        mov     [mv_src_off+2], ds
        mov     ax, [handle]
        mov     [mv_dst_h], ax                  ; to the block
        mov     word [mv_dst_off], 0
        mov     word [mv_dst_off+2], 0
        mov     ah, 0x0B
        mov     si, move_struct
        call    far [entry]
        or      ax, ax
        jz      .move_bad

        mov     word [mv_src_h], 0
        mov     ax, [handle]
        mov     [mv_src_h], ax                  ; back from the block
        mov     word [mv_src_off], 0
        mov     word [mv_src_off+2], 0
        mov     word [mv_dst_h], 0
        mov     word [mv_dst_off], buf_in
        mov     [mv_dst_off+2], ds
        mov     ah, 0x0B
        mov     si, move_struct
        call    far [entry]
        or      ax, ax
        jz      .move_bad

        mov     si, buf_out
        mov     di, buf_in
        mov     cx, 512
        cld
        repe    cmpsb
        jne     .differs
        mov     si, msg_same
        call    puts
        jmp     .free
.differs:
        mov     si, msg_differs
        call    puts
        jmp     .free
.move_bad:
        mov     si, msg_movebad
        call    puts
        mov     al, bl
        call    put_hex8
        call    crlf
.free:
        mov     ah, 0x0A
        mov     dx, [handle]
        call    far [entry]
.bye:
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------- printing
putc:   push    ax
        push    bx
        mov     ah, 0x0E
        xor     bx, bx
        int     0x10
        pop     bx
        pop     ax
        ret

puts:   push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:  pop     ax
        ret

crlf:   push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
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

put_hex16:
        push    ax
        push    ax
        mov     al, ah
        call    put_hex8
        pop     ax
        call    put_hex8
        pop     ax
        ret

; AX as decimal, no padding
put_dec:
        push    ax
        push    bx
        push    cx
        push    dx
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
msg_head:    db "XMSTEST: extended memory, as a DOS program sees it", 13, 10, 13, 10, 0
msg_none:    db "No XMS driver answered INT 2Fh.", 13, 10, 0
msg_present: db "An XMS driver answered.", 13, 10, 0
msg_entry:   db "  entry point      ", 0
msg_version: db "  version          ", 0
msg_largest: db "  largest block    ", 0
msg_total:   db "  free in total    ", 0
msg_kb:      db " KB", 13, 10, 0
msg_handle:  db "  a 64 KB block    handle ", 0
msg_noalloc: db "  it would not give one out", 13, 10, 0
msg_movebad: db "  the move failed, error ", 0
msg_same:    db "  512 bytes went up and came back unchanged: it works", 13, 10, 0
msg_differs: db "  what came back is not what went up", 13, 10, 0

entry:       dd 0
handle:      dw 0

move_struct:
mv_len:      dd 0
mv_src_h:    dw 0
mv_src_off:  dd 0
mv_dst_h:    dw 0
mv_dst_off:  dd 0

buf_out:     times 512 db 0
buf_in:      times 512 db 0
