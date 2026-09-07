; =============================================================================
;  log.asm - boot/diagnostic log kept in memory and written to \EMBER.LOG
; -----------------------------------------------------------------------------
;  The build plants a fixed-size EMBER.LOG on the image.  log_flush rewrites
;  that file's sectors in place, so no FAT or directory changes are needed and
;  the text can be read on any PC after the stick is plugged back in.
; =============================================================================

LOG_MAX         equ 0x2000                      ; 8 KB at [log_seg]

log_reset:
        mov     word [log_len], 0
        ret

; log_putc: append AL
log_putc:
        push    bx
        push    es
        mov     es, [log_seg]
        mov     bx, [log_len]
        cmp     bx, LOG_MAX
        jb      .store
        ; full: drop the oldest half so recent lines survive
        push    cx
        push    si
        push    di
        push    ds
        mov     ds, [log_seg]
        mov     si, LOG_MAX / 2
        xor     di, di
        mov     cx, LOG_MAX / 4
        rep     movsw
        pop     ds
        pop     di
        pop     si
        pop     cx
        mov     bx, LOG_MAX / 2
        mov     word [cs:log_len], bx
.store: mov     [es:bx], al
        inc     word [log_len]
        pop     es
        pop     bx
        ret

; log_puts: append NUL-terminated DS:SI
log_puts:
        push    ax
        push    si
.next:  lodsb
        or      al, al
        jz      .done
        call    log_putc
        jmp     .next
.done:  pop     si
        pop     ax
        ret

log_crlf:
        push    ax
        mov     al, 13
        call    log_putc
        mov     al, 10
        call    log_putc
        pop     ax
        ret

; log_hex32: append EAX as 8 hex digits
log_hex32:
        push    eax
        push    cx
        mov     cx, 8
.digit: rol     eax, 4
        push    eax
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    log_putc
        pop     eax
        loop    .digit
        pop     cx
        pop     eax
        ret

; log_dec: append EAX in decimal
log_dec:
        pushad
        mov     ebx, 10
        xor     cx, cx
.div:   xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .div
.emit:  pop     ax
        add     al, '0'
        call    log_putc
        loop    .emit
        popad
        ret

; log_line: append DS:SI, then EAX in hex, then CR LF
log_line:
        call    log_puts
        call    log_hex32
        call    log_crlf
        ret

; log_text: append DS:SI and CR LF
log_text:
        call    log_puts
        call    log_crlf
        ret

; -----------------------------------------------------------------------------
; log_flush: write the log over \EMBER.LOG (padding with spaces).
;   Silently does nothing if the file is missing or the filesystem is absent.
; -----------------------------------------------------------------------------
log_flush:
        pushad
        push    es
        push    ds
        cmp     byte [fs_ok], 0
        je      .done
        mov     si, log_file_name
        call    resolve_path
        jc      .done
        mov     ax, ds
        mov     es, ax
        mov     eax, [found_size]
        mov     [lf_remaining], eax
        mov     ax, [found_cluster]
        cmp     ax, 2
        jb      .done
        mov     word [lf_pos], 0
.cluster:
        push    ax
        call    cluster_to_lba
        mov     [lf_lba], eax
        movzx   dx, byte [fs_spc]
.sector:
        cmp     dword [lf_remaining], 0
        je      .done_pop
        ; build one sector: log bytes, then spaces
        mov     di, sector_buf
        mov     cx, 512
.fill:  mov     al, ' '
        mov     bx, [lf_pos]
        cmp     bx, [log_len]
        jae     .store
        mov     ax, [log_seg]
        push    ds
        mov     ds, ax
        mov     al, [bx]
        pop     ds
.store: mov     [di], al
        inc     di
        inc     word [lf_pos]
        loop    .fill
        mov     eax, [lf_lba]
        mov     bx, sector_buf
        call    write_sector
        jc      .done_pop
        inc     dword [lf_lba]
        sub     dword [lf_remaining], 512
        jnc     .more
        mov     dword [lf_remaining], 0
.more:  dec     dx
        jnz     .sector
        pop     ax
        call    next_cluster
        jc      .stamp
        jmp     .cluster
.done_pop:
        pop     ax
.stamp:
        ; The bytes are written in place, so the entry itself never changes and
        ; a host that caches a file by its size and time will keep showing the
        ; old text.  Put the time of day on it, so the change is visible.
        mov     eax, [found_dir_lba]
        or      eax, eax
        jz      .done
        call    dir_load
        jc      .done
        call    fat_now                         ; AX = time, DX = date
        mov     bx, [found_dir_off]
        mov     [dir_buf+bx+22], ax
        mov     [dir_buf+bx+24], dx
        mov     eax, [found_dir_lba]
        call    dir_store
.done:  pop     ds
        pop     es
        popad
        ret

section .data
log_len:        dw 0
log_file_name:  db "\EMBER.LOG", 0
lf_pos:         dw 0
                align 4
lf_remaining:   dd 0
lf_lba:         dd 0
section .text
