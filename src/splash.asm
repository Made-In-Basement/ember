; =============================================================================
;  splash.asm - SPLASH file [wavfile]: a full-screen picture, usually at boot
; -----------------------------------------------------------------------------
;  The file is made by tools/mksplash.py: "NSPL", width and height, a
;  256-entry palette, then (count, colour) runs.  The picture is drawn in
;  800x600, a WAV file may be played underneath it, and it stays up until a
;  key is pressed or about three seconds have passed.  A card that will not
;  give us 800x600 gets no splash rather than a cropped one.
; =============================================================================

SPL_SCRATCH_PARAS equ 0x100                     ; 4 KB from the arena
SPL_CHUNK       equ 2048                        ; runs read this many bytes at a time
SPL_HOLD_TICKS  equ 55                          ; about three seconds
SPL_MAX_W       equ 1920
spl_row         equ nx_rm_stack                 ; idle unless a 32-bit program runs
spl_out         equ nx_rm_stack + 2048          ; the widened copy of it

cmd_splash:
        call    next_arg
        jc      .usage
        mov     di, copy_src
        call    copy_arg
        mov     byte [spl_has_wav], 0
        call    next_arg
        jc      .no_wav
        mov     di, copy_dst
        call    copy_arg
        mov     byte [spl_has_wav], 1
.no_wav:
        mov     si, copy_src
        call    resolve_path
        jc      .not_found
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     .not_found
        mov     bx, SPL_SCRATCH_PARAS
        call    mem_alloc_system
        jc      .no_memory
        mov     [spl_seg], ax
        call    fstream_open
        ; ---- the header and the palette ----
        mov     es, [spl_seg]
        xor     di, di
        mov     ecx, 8 + 768
        call    fstream_read
        mov     es, [spl_seg]
        cmp     dword [es:0], "NSPL"
        jne     .bad_file
        mov     ax, [es:4]
        cmp     ax, SPL_MAX_W
        ja      .bad_file
        mov     [spl_w], ax
        mov     ax, [es:6]
        mov     [spl_h], ax
        ; ---- the screen ----
        mov     al, 5                           ; the widest mode on offer
        call    gfx_init
        cmp     byte [gfx_ok], 0
        je      .no_gfx
        ; How many source pixels to step per screen pixel, as 16.16.  This
        ; needs 32 bits: a picture wider than the screen steps by more than
        ; one, which will not fit in a 16-bit fraction.
        movzx   eax, word [spl_w]
        shl     eax, 16
        xor     edx, edx
        movzx   ecx, word [scr_w]
        div     ecx
        mov     [spl_xstep], eax
        call    gfx_set_palette                 ; finds out the DAC width
        call    spl_palette
        call    spl_draw
        ; ---- hold it: the music first, then a key or the clock ----
        call    spl_ticks
        mov     [spl_start], eax
        cmp     byte [spl_has_wav], 0
        je      .hold
        mov     si, copy_dst
        call    resolve_path
        jc      .hold
        call    play_wav
.hold:  call    kbhit
        jnz     .key
        hlt
        call    spl_ticks
        sub     eax, [spl_start]
        cmp     eax, SPL_HOLD_TICKS
        jb      .hold
        jmp     .leave
.key:   call    getkey
.leave: call    gfx_done
        call    cls
        jmp     .free
.wrong_size:
        call    gfx_done
        call    cls
        jmp     .free
.no_gfx:
.bad_file:
.free:  mov     es, [spl_seg]
        call    mem_free
        push    cs
        pop     es
        ret
.no_memory:
        mov     si, msg_gui_no_mem
        call    puts
        ret
.not_found:
        mov     si, msg_splash_missing
        call    puts
        ret
.usage: mov     si, msg_splash_usage
        call    puts
        ret

; spl_ticks: EAX = the BIOS clock
spl_ticks:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     eax, [es:0x046C]
        pop     es
        ret

; spl_palette: the 256 entries at ES:8, 8-bit values, into the DAC
spl_palette:
        pusha
        mov     si, 8
        mov     cx, 768
        mov     bl, [dac_shift]
        mov     dx, 0x3C8
        xor     al, al
        out     dx, al
        inc     dx
.next:  mov     al, [es:si]
        inc     si
        xchg    cl, bl
        shr     al, cl
        xchg    cl, bl
        out     dx, al
        loop    .next
        popa
        ret

; spl_draw: decode the runs into a row at a time and put each row up.
;   ES = the scratch segment; the stream is positioned after the palette.
spl_draw:
        pusha
        mov     word [spl_left], 0
        mov     word [spl_x], 0
        mov     word [spl_y], 0
        xor     si, si
        xor     bp, bp
.run:   cmp     word [spl_left], 2
        jae     .have
        call    spl_refill
        jc      .done
        xor     si, si
.have:  mov     al, [es:si]
        mov     ah, [es:si+1]
        add     si, 2
        sub     word [spl_left], 2
        movzx   bp, al                          ; BP = pixels still in this run
        mov     [spl_colour], ah
.fill:  mov     cx, [spl_w]
        sub     cx, [spl_x]                     ; room left in the row
        cmp     cx, bp
        jbe     .clip
        mov     cx, bp
.clip:  mov     di, spl_row
        add     di, [spl_x]
        add     [spl_x], cx
        sub     bp, cx
        mov     al, [spl_colour]
        push    es
        push    ds
        pop     es
        rep     stosb
        pop     es
        mov     ax, [spl_x]
        cmp     ax, [spl_w]
        jb      .more
        ; ---- a whole source row: stretch it and put it on every screen
        ;      row it covers, so the picture fills the mode it got ----
        push    si
        call    spl_emit_row
        pop     si
        mov     word [spl_x], 0
        inc     word [spl_y]
        mov     ax, [spl_y]
        cmp     ax, [spl_h]
        jae     .done
.more:  test    bp, bp
        jnz     .fill
        jmp     .run
.done:  popa
        ret

; spl_emit_row: source row [spl_y] is complete in spl_row.  Widen it to the
;   screen and draw it on every screen row it covers.
spl_emit_row:
        pusha
        push    es                              ; the caller reads its runs
        push    ds                              ;  through ES: leave it alone
        ; which screen rows does this source row cover?
        mov     ax, [spl_y]
        mul     word [scr_h]
        div     word [spl_h]
        mov     [spl_dy0], ax
        mov     ax, [spl_y]
        inc     ax
        mul     word [scr_h]
        div     word [spl_h]
        cmp     ax, [spl_dy0]
        jne     .have_span
        inc     ax                              ; always at least one row
.have_span:
        mov     [spl_dy1], ax
        ; widen the row once
        push    ds
        pop     es
        mov     di, spl_out
        mov     cx, [scr_w]
        xor     ebx, ebx                        ; where we are in the source
.widen: mov     eax, ebx
        shr     eax, 16                         ; the whole part of it
        mov     si, spl_row
        add     si, ax
        mov     al, [si]
        stosb
        add     ebx, [spl_xstep]
        loop    .widen
        ; and lay it down
        mov     bx, [spl_dy0]
.rows:  cmp     bx, [spl_dy1]
        jae     .done
        cmp     bx, [scr_h]
        jae     .done
        push    bx
        xor     ax, ax
        mov     cx, [scr_w]
        mov     si, spl_out
        call    gfx_blit_row
        pop     bx
        inc     bx
        jmp     .rows
.done:  pop     ds
        pop     es
        popa
        ret

; spl_refill: the next chunk of runs into ES:0.  CF=1 when the file is used up
spl_refill:
        push    eax
        push    ecx
        push    di
        mov     eax, [fs_left]
        test    eax, eax
        jz      .eof
        mov     ecx, SPL_CHUNK
        cmp     eax, ecx
        jae     .whole
        mov     ecx, eax
.whole: mov     [spl_left], cx
        xor     di, di
        call    fstream_read
        mov     es, [spl_seg]                   ; the read may move ES along
        pop     di
        pop     ecx
        pop     eax
        clc
        ret
.eof:   pop     di
        pop     ecx
        pop     eax
        stc
        ret

msg_splash_usage:   db "Usage: SPLASH picture.bin [music.wav]", 13, 10, 0
msg_splash_missing: db "SPLASH: picture not found", 13, 10, 0
