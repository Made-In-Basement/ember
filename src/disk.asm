; =============================================================================
;  disk.asm - sector reads through the BIOS (LBA with CHS fallback)
; =============================================================================

; -----------------------------------------------------------------------------
; disk_init: detect INT 13h extensions and the drive geometry of [boot_drive]
; -----------------------------------------------------------------------------
disk_init:
        pusha
        push    es
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [boot_drive]
        int     0x13
        jc      .no_lba
        cmp     bx, 0xAA55
        jne     .no_lba
        test    cx, 1
        jz      .no_lba
        mov     byte [use_lba], 1
.no_lba:
        mov     ah, 0x08
        mov     dl, [boot_drive]
        xor     di, di
        mov     es, di
        int     0x13
        jc      .done
        and     cx, 0x3F
        jz      .done
        mov     [disk_spt], cx
        mov     dl, dh
        xor     dh, dh
        inc     dx
        mov     [disk_heads], dx
.done:
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; read_sector: EAX = LBA (relative to the volume), ES:BX = buffer.
;   Returns CF=1 on failure (after 3 retries).  Preserves all registers.
; -----------------------------------------------------------------------------
read_sector:
        mov     byte [disk_op], 0
        jmp     disk_io
; write_sector: same interface, writes the sector (used for the log file)
write_sector:
        mov     byte [disk_op], 1
disk_io:
        pushad
        add     eax, [fs_hidden]        ; volume LBA -> disk LBA
        mov     byte [disk_retries], 3
.retry:
        cmp     byte [use_lba], 0
        je      .chs
        mov     [dap_lba], eax
        mov     [dap_off], bx
        mov     [dap_seg], es
        mov     si, dap
        mov     ah, 0x42                ; 42h = read, 43h = write
        add     ah, [disk_op]
        xor     al, al
        mov     dl, [boot_drive]
        int     0x13
        jnc     .ok
        jmp     .fail
.chs:
        push    eax
        push    bx
        xor     edx, edx
        movzx   ecx, word [disk_spt]
        div     ecx                     ; EAX = LBA/SPT, EDX = LBA%SPT
        inc     dl
        mov     cl, dl                  ; sector
        xor     edx, edx
        movzx   ebx, word [disk_heads]
        div     ebx                     ; EAX = cylinder, EDX = head
        mov     dh, dl                  ; head
        mov     ch, al                  ; cylinder 7:0
        shl     ah, 6
        or      cl, ah                  ; cylinder 9:8
        pop     bx
        mov     dl, [boot_drive]
        mov     ah, 0x02                ; 02h = read, 03h = write
        add     ah, [disk_op]
        mov     al, 1
        int     0x13
        pop     eax
        jnc     .ok
.fail:
        dec     byte [disk_retries]
        jz      .error
        push    ax
        xor     ah, ah
        mov     dl, [boot_drive]
        int     0x13                    ; reset drive
        pop     ax
        jmp     .retry
.ok:
        popad
        clc
        ret
.error:
        popad
        stc
        ret

; -----------------------------------------------------------------------------
; read_sectors: EAX = first LBA, CX = count, ES:BX = buffer (advances across
;   64 KB boundaries).  CF=1 on error.  Preserves registers.
; -----------------------------------------------------------------------------
read_sectors:
        pushad
        push    es
        jcxz    .ok
.next:  call    read_sector
        jc      .error
        inc     eax
        add     bx, 512
        jnc     .no_wrap
        mov     dx, es
        add     dx, 0x1000
        mov     es, dx
.no_wrap:
        loop    .next
.ok:    pop     es
        popad
        clc
        ret
.error: pop     es
        popad
        stc
        ret

; -----------------------------------------------------------------------------
; data
; -----------------------------------------------------------------------------
section .data
use_lba:        db 0
disk_spt:       dw 18
disk_heads:     dw 2
disk_retries:   db 0
disk_op:        db 0
        align 4
dap:            db 0x10, 0
dap_count:      dw 1
dap_off:        dw 0
dap_seg:        dw 0
dap_lba:        dq 0
section .text
