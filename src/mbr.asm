; =============================================================================
;  Ember master boot record  (src/mbr.asm)
; -----------------------------------------------------------------------------
;  Sector 0 of a partitioned USB image.  Relocates itself to 0000:0600, finds
;  the active partition in the table at offset 1BEh, loads that partition's
;  boot sector (src/boot.asm with hidden sectors set) to 0000:7C00 and jumps
;  to it with DL = drive and DS:SI -> the partition entry, as every PC does.
; =============================================================================

[BITS 16]
[ORG 0x0600]

        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        cld
        mov     si, 0x7C00                      ; copy ourselves out of the way
        mov     di, 0x0600
        mov     cx, 256
        rep     movsw
        jmp     0x0000:relocated

relocated:
        mov     [drive], dl
        mov     si, 0x0600 + 0x1BE
        mov     cx, 4
.find:  test    byte [si], 0x80
        jnz     .found
        add     si, 16
        loop    .find
        mov     si, msg_no_part
        jmp     fail
.found:
        mov     [part_entry], si
        mov     eax, [si+8]                     ; partition start LBA

        ; INT 13h extensions?
        push    eax
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [drive]
        int     0x13
        jc      .no_lba
        cmp     bx, 0xAA55
        jne     .no_lba
        test    cx, 1
        jz      .no_lba
        mov     byte [use_lba], 1
.no_lba:
        ; drive geometry for CHS
        mov     ah, 0x08
        mov     dl, [drive]
        xor     di, di
        int     0x13
        jc      .geom_done
        and     cx, 0x3F
        jz      .geom_done
        mov     [spt], cx
        mov     dl, dh
        xor     dh, dh
        inc     dx
        mov     [heads], dx
.geom_done:
        xor     ax, ax
        mov     es, ax
        pop     eax

        mov     bx, 0x7C00
        mov     byte [retries], 3
.retry:
        cmp     byte [use_lba], 0
        je      .chs
        mov     [dap_lba], eax
        mov     si, dap
        mov     ah, 0x42
        mov     dl, [drive]
        int     0x13
        jnc     .loaded
        jmp     .again
.chs:   push    eax
        xor     edx, edx
        movzx   ecx, word [spt]
        div     ecx
        inc     dl
        mov     cl, dl
        xor     edx, edx
        movzx   ebx, word [heads]
        div     ebx
        mov     dh, dl
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     dl, [drive]
        mov     bx, 0x7C00
        mov     ax, 0x0201
        int     0x13
        pop     eax
        jnc     .loaded
.again: dec     byte [retries]
        jz      .read_error
        push    ax
        xor     ah, ah
        mov     dl, [drive]
        int     0x13
        pop     ax
        jmp     .retry
.read_error:
        mov     si, msg_read
        jmp     fail
.loaded:
        cmp     word [0x7DFE], 0xAA55
        jne     .bad_sig
        mov     dl, [drive]
        mov     si, [part_entry]
        jmp     0x0000:0x7C00
.bad_sig:
        mov     si, msg_sig

fail:   ; print DS:SI and wait for a key
        push    si
.print: lodsb
        or      al, al
        jz      .wait
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .print
.wait:  xor     ax, ax
        int     0x16
        int     0x19

msg_no_part:    db "No active partition", 0
msg_read:       db "MBR: disk read error", 0
msg_sig:        db "Missing boot signature", 0

drive:          db 0
use_lba:        db 0
retries:        db 0
spt:            dw 63
heads:          dw 255
part_entry:     dw 0
        align 4
dap:            db 0x10, 0
                dw 1
                dw 0x7C00
                dw 0
dap_lba:        dq 0

        times 446-($-$$) db 0
partition_table:
        times 64 db 0                           ; filled in by build.py
        dw 0xAA55
