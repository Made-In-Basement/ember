; =============================================================================
;  DPMITEST.COM - is there a DPMI host, and does it keep its promises?
; -----------------------------------------------------------------------------
;  Enters the host as a 32-bit client the way an extender would, then tries
;  the services one by one: version, descriptors, extended memory, DOS
;  memory, a real-mode interrupt, an exception handler, a hooked hardware
;  interrupt, a real-mode callback, the virtual interrupt flag.  Each line
;  says what was tried and whether it went as the specification says.
;  The code stays 16-bit; a 32-bit client only means the host's frames and
;  structures are the 32-bit ones, which is what this checks.
; =============================================================================

[BITS 16]
[ORG 0x0100]

start:
        mov     [rm_seg], cs
        mov     si, msg_head
        call    puts_rm

        ; ---- is there a host? ----
        mov     ax, 0x1687
        int     0x2F
        or      ax, ax
        jz      .present
        mov     si, msg_none
        call    puts_rm
        jmp     exit_rm
.present:
        mov     [entry], di
        mov     [entry+2], es
        mov     [host_paras], si
        mov     [host_flags], bx
        mov     [host_ver], dx
        mov     si, msg_present
        call    puts_rm
        mov     al, dh
        call    put_dec_rm
        mov     al, '.'
        call    putc_rm
        mov     al, dl
        call    put_dec_rm
        mov     si, msg_flags
        call    puts_rm
        mov     ax, [host_flags]
        call    put_hex16_rm
        call    crlf_rm
        test    byte [host_flags], 1
        jnz     .can32
        mov     si, msg_no32
        call    puts_rm
        jmp     exit_rm
.can32:
        ; ---- memory for the host's data, as a real client would ----
        mov     ax, cs                          ; (the check left ES on the host)
        mov     es, ax
        mov     bx, 0x1000                      ; give the rest of memory back
        mov     ah, 0x4A
        int     0x21
        mov     bx, 0x0100                      ; can DOS memory be had at all?
        mov     ah, 0x48
        int     0x21
        jc      .rm_alloc_bad
        mov     es, ax
        mov     ah, 0x49
        int     0x21
        jmp     .rm_alloc_ok
.rm_alloc_bad:
        mov     si, msg_rm_alloc
        call    puts_rm
.rm_alloc_ok:
        mov     bx, [host_paras]
        or      bx, bx
        jz      .no_data
        mov     ah, 0x48
        int     0x21
        jc      .no_data
        mov     es, ax
.no_data:
        ; ---- in we go ----
        mov     ax, 1                           ; a 32-bit client
        call    far [entry]
        jnc     pm_start
        mov     si, msg_refused
        call    puts_rm
exit_rm:
        mov     ax, 0x4C00
        int     0x21

; =============================================================================
; protected mode from here: CS is a selector for this segment
; =============================================================================
pm_start:
        mov     [pm_ds], ds
        mov     [pm_cs], cs
        mov     [pm_ss], ss
        mov     [pm_es], es
        mov     si, msg_in_pm
        call    puts
        mov     ax, cs
        call    put_hex16
        mov     al, ' '
        call    putc
        mov     ax, ds
        call    put_hex16
        mov     al, ' '
        call    putc
        mov     ax, ss
        call    put_hex16
        mov     al, ' '
        call    putc
        mov     ax, es
        call    put_hex16
        call    crlf
        mov     ax, [pm_es]                     ; the PSP selector: its bytes
        mov     es, ax
        cmp     word [es:0], 0x20CD             ; INT 20h, as every PSP starts
        mov     si, msg_psp
        call    check                           ; ZF = ok

        ; ---- 0400: version ----
        mov     ax, 0x0400
        int     0x31
        push    ax
        mov     si, msg_version
        call    puts
        pop     ax
        call    put_hex16
        mov     si, msg_version2
        call    puts
        mov     ax, bx
        call    put_hex16
        call    crlf

        ; ---- 0000 / 0007 / 0008: a descriptor onto the text screen ----
        mov     ax, 0x0000
        mov     cx, 1
        int     0x31
        jc      .desc_fail
        mov     [sel_scr], ax
        mov     bx, ax
        mov     ax, 0x0007
        mov     cx, 0x000B
        mov     dx, 0x8000                      ; base B8000h
        int     0x31
        jc      .desc_fail
        mov     ax, 0x0008
        xor     cx, cx
        mov     dx, 0x0FFF
        int     0x31
        jc      .desc_fail
        mov     ax, 0x0006                      ; and read it back
        int     0x31
        jc      .desc_fail
        cmp     cx, 0x000B
        jne     .desc_fail
        cmp     dx, 0x8000
        jne     .desc_fail
        mov     es, bx
        mov     word [es:2*78], 0x2F50          ; "PM" top right, in green
        mov     word [es:2*79], 0x2F4D
        mov     si, msg_desc
        xor     ax, ax                          ; ZF set: ok
        call    check
        jmp     .desc_done
.desc_fail:
        mov     si, msg_desc
        or      sp, sp                          ; ZF clear
        call    check
.desc_done:

        ; ---- 0501: a megabyte of extended memory, written and read ----
        mov     ax, 0x0501
        mov     bx, 0x0010
        xor     cx, cx                          ; 100000h bytes
        int     0x31
        jc      .mem_fail
        mov     [mem_handle], di
        mov     [mem_handle+2], si
        mov     [mem_base], cx
        mov     [mem_base+2], bx
        mov     ax, 0x0000
        mov     cx, 1
        int     0x31
        jc      .mem_fail
        mov     [sel_mem], ax
        mov     bx, ax
        mov     ax, 0x0007
        mov     cx, [mem_base+2]
        mov     dx, [mem_base]
        int     0x31
        jc      .mem_fail
        mov     ax, 0x0008
        mov     cx, 0x000F
        mov     dx, 0xFFFF                      ; limit FFFFFh
        int     0x31
        jc      .mem_fail
        mov     es, [sel_mem]
        mov     edi, 0xFFFF0
        mov     dword [es:0], 0x12345678
        mov     dword [es:edi], 0xCAFEF00D
        cmp     dword [es:0], 0x12345678
        jne     .mem_fail
        cmp     dword [es:edi], 0xCAFEF00D
        jne     .mem_fail
        mov     ax, 0x0502
        mov     di, [mem_handle]
        mov     si, [mem_handle+2]
        int     0x31
        jc      .mem_fail
        mov     si, msg_mem
        xor     ax, ax
        call    check
        jmp     .mem_done
.mem_fail:
        mov     si, msg_mem
        or      sp, sp
        call    check
.mem_done:

        ; ---- 0500: how much is there ----
        mov     ax, 0x0500
        push    ds
        pop     es
        mov     edi, mem_info
        int     0x31
        mov     si, msg_free
        call    puts
        mov     eax, [mem_info]
        shr     eax, 10
        call    put_dec32
        mov     si, msg_kb
        call    puts

        ; ---- 0100 / 0101: DOS memory through a selector ----
        mov     ax, 0x0100
        mov     bx, 0x0100                      ; 4 KB
        int     0x31
        jc      .dos_fail
        mov     [dos_sel], dx
        mov     [dos_seg], ax
        mov     es, dx
        mov     word [es:0], 0xBEEF
        mov     word [es:0xFFE], 0x1234
        cmp     word [es:0], 0xBEEF
        jne     .dos_fail
        mov     ax, 0x0101
        mov     dx, [dos_sel]
        int     0x31
        jc      .dos_fail
        mov     si, msg_dos
        xor     ax, ax
        call    check
        jmp     .dos_done
.dos_fail:
        mov     si, msg_dos
        or      sp, sp
        call    check
.dos_done:

        ; ---- 0300: a real-mode interrupt with registers going both ways ----
        ;   INT 21h AH=30h answers with the DOS version in AL.AH; BX, CX too
        push    ds
        pop     es
        mov     edi, rmcs
        call    rmcs_clear
        mov     dword [rmcs+28], 0x00003000     ; EAX
        mov     ax, 0x0300
        mov     bl, 0x21
        xor     bh, bh
        xor     cx, cx
        int     0x31
        jc      .rm_fail
        mov     si, msg_rmint
        call    puts
        mov     eax, [rmcs+28]
        call    put_hex16
        mov     si, msg_rmint2
        call    puts
        cmp     byte [rmcs+28], 0               ; a version, not zero
        je      .rm_fail
        mov     si, msg_rmint3
        xor     ax, ax
        call    check
        jmp     .rm_done
.rm_fail:
        mov     si, msg_rmint3
        or      sp, sp
        call    check
.rm_done:

        ; ---- 0203: an exception handler that steps over a division by zero ----
        mov     ax, 0x0202
        mov     bl, 0
        int     0x31
        mov     [exc_old], edx
        mov     [exc_old+4], cx
        mov     ax, 0x0203
        mov     bl, 0
        mov     cx, cs
        mov     edx, exc_handler
        int     0x31
        jc      .exc_fail
        mov     byte [exc_hit], 0
        xor     cx, cx
        mov     ax, 7
        xor     dx, dx
        div     cx                              ; #DE: the handler skips it
        cmp     byte [exc_hit], 1
        jne     .exc_fail
        mov     ax, 0x0203                      ; and put the old one back
        mov     bl, 0
        mov     cx, [exc_old+4]
        mov     edx, [exc_old]
        int     0x31
        mov     si, msg_exc
        xor     ax, ax
        call    check
        jmp     .exc_done
.exc_fail:
        mov     si, msg_exc
        or      sp, sp
        call    check
.exc_done:

        ; ---- 0204 / 0205: the timer, hooked in protected mode and chained ----
        mov     ax, 0x0204
        mov     bl, 8
        int     0x31
        mov     [old8], edx
        mov     [old8+4], cx
        mov     word [ticks], 0
        mov     ax, 0x0205
        mov     bl, 8
        mov     cx, cs
        mov     edx, timer_handler
        int     0x31
        jc      .irq_fail
        sti
        ; Four ticks is 220 ms.  The old budget of 4000000h ran out at
        ; three often enough to fail a good host, and a real processor
        ; spins it faster than QEMU does, so it is generous now: a
        ; host that never delivers a tick still gives up in seconds.
        mov     ecx, 0x40000000                 ; patience, then give up
.tick_wait:
        cmp     word [ticks], 4
        jae     .ticked
        dec     ecx
        jnz     .tick_wait
.ticked:
        mov     ax, 0x0205                      ; the old one back
        mov     bl, 8
        mov     cx, [old8+4]
        mov     edx, [old8]
        int     0x31
        cmp     word [ticks], 4
        jb      .irq_fail
        mov     si, msg_irq
        xor     ax, ax
        call    check
        jmp     .irq_done
.irq_fail:
        mov     ax, [ticks]
        mov     si, msg_irq
        or      sp, sp
        call    check
.irq_done:

        ; ---- 0303: a real-mode callback, reached from a real-mode call ----
        mov     ax, 0x0303
        push    ds
        pop     es
        mov     edi, cb_rmcs                    ; ES:EDI = the structure
        mov     esi, cb_proc                    ; DS:ESI = the procedure
        push    ds
        push    cs
        pop     ds
        int     0x31
        pop     ds
        jc      .cb_fail
        mov     [cb_addr], dx
        mov     [cb_addr+2], cx
        mov     byte [cb_hit], 0
        ; a real-mode routine that calls the callback and returns
        push    ds
        pop     es
        mov     edi, rmcs
        call    rmcs_clear
        mov     ax, [rm_seg]
        mov     [rmcs+44], ax                   ; CS
        mov     word [rmcs+42], rm_caller       ; IP
        mov     ax, 0x0301
        xor     bh, bh
        xor     cx, cx
        int     0x31
        jc      .cb_fail
        cmp     byte [cb_hit], 1
        jne     .cb_fail
        cmp     dword [cb_rmcs+28], 0x00001234  ; EAX the real-mode side set
        jne     .cb_fail
        mov     ax, 0x0304
        mov     dx, [cb_addr]
        mov     cx, [cb_addr+2]
        int     0x31
        jc      .cb_fail
        mov     si, msg_cb
        xor     ax, ax
        call    check
        jmp     .cb_done
.cb_fail:
        mov     si, msg_cb
        or      sp, sp
        call    check
.cb_done:

        ; ---- 0900 / 0901: the virtual interrupt flag ----
        mov     ax, 0x0900                      ; disable, get the old state
        int     0x31
        cmp     al, 1
        jne     .vif_fail
        mov     ax, 0x0902
        int     0x31
        cmp     al, 0
        jne     .vif_fail
        mov     ax, 0x0901
        int     0x31
        cmp     al, 0
        jne     .vif_fail
        mov     ax, 0x0902
        int     0x31
        cmp     al, 1
        jne     .vif_fail
        cli                                     ; trapped and applied
        mov     ax, 0x0902
        int     0x31
        cmp     al, 0
        jne     .vif_fail
        sti
        mov     ax, 0x0902
        int     0x31
        cmp     al, 1
        jne     .vif_fail
        mov     si, msg_vif
        xor     ax, ax
        call    check
        jmp     .vif_done
.vif_fail:
        mov     si, msg_vif
        or      sp, sp
        call    check
.vif_done:

        ; ---- and out, through DOS as any program ----
        mov     si, msg_bye
        call    puts
        mov     ax, 0x4C00
        int     0x21

; ---- the exception handler: skip the two-byte DIV, note the visit ----
;   A handler arrives with whatever DS the interrupted code had, so it loads
;   its own from a read through CS.  It may not *write* through CS: a code
;   segment is never writable, whatever its R bit says, and the host has no
;   reason to hand out an alias.  The push and pop are balanced before the
;   frame is touched, so the offsets below stay as they are.
exc_handler:
        push    ds
        mov     ds, [cs:pm_ds]
        mov     byte [exc_hit], 1
        pop     ds
        mov     eax, [esp+12]                   ; EIP in the frame
        add     eax, 2
        mov     [esp+12], eax
        o32 retf

; ---- the timer handler: count, then the previous handler ----
timer_handler:
        push    ds
        mov     ds, [cs:pm_ds]
        inc     word [ticks]
        pop     ds
        jmp     far dword [cs:old8]

; ---- the callback's procedure: pop the return address, go back ----
cb_proc:
        push    ds
        mov     ds, [cs:pm_ds]                  ; DS arrives as the structure's
        mov     byte [cb_hit], 1                ;  selector: put it back
        pop     ds
        mov     ax, [es:edi]                    ; IP on the real-mode stack
        mov     [esi+42], ax
        mov     ax, [es:edi+2]                  ; CS
        mov     [esi+44], ax
        add     word [esi+46], 4                ; SP past them
        o32 iret

; ---- real mode: call the callback, then return to the host ----
rm_caller:
        mov     eax, 0x1234
        call    far [cs:cb_addr]
        retf

; =============================================================================
; helpers, protected mode: printing goes through INT 21h in real mode
; =============================================================================
; check: DS:SI = the test's name; ZF set = ok
check:
        pushf
        call    puts
        popf
        jnz     .fail
        mov     si, msg_ok
        call    puts
        ret
.fail:  push    ax
        mov     si, msg_fail
        call    puts
        pop     ax
        call    put_hex16                       ; AX, an error code if it was one
        mov     al, ' '
        call    putc
        mov     ax, bx
        call    put_hex16
        call    crlf
        inc     byte [failures]
        ret

; puts: DS:SI, NUL-terminated (copied to a $-terminated buffer)
puts:
        pushad
        push    es
        mov     di, out_buf
.copy:  lodsb
        or      al, al
        jz      .end
        mov     [di], al
        inc     di
        cmp     di, out_buf + 250
        jb      .copy
.end:   mov     byte [di], '$'
        push    ds
        pop     es
        mov     edi, rmcs
        call    rmcs_clear
        mov     ax, [rm_seg]
        mov     [rmcs+36], ax                   ; DS
        mov     dword [rmcs+20], out_buf        ; EDX
        mov     dword [rmcs+28], 0x00000900     ; EAX
        mov     ax, 0x0300
        mov     bl, 0x21
        xor     bh, bh
        xor     cx, cx
        int     0x31
        pop     es
        popad
        ret

rmcs_clear:                                     ; ES:DI -> 50 zero bytes
        push    cx
        push    di
        push    ax
        mov     cx, 25
        xor     ax, ax
        rep     stosw
        pop     ax
        pop     di
        pop     cx
        ret

putc:
        push    si
        mov     [char_buf], al
        mov     si, char_buf
        call    puts
        pop     si
        ret

crlf:
        push    si
        mov     si, msg_crlf
        call    puts
        pop     si
        ret

put_hex16:
        pushad
        mov     cx, 4
.digit: rol     ax, 4
        push    ax
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    putc
        pop     ax
        loop    .digit
        popad
        ret

put_dec32:                                      ; EAX
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
        call    putc
        loop    .emit
        popad
        ret

; =============================================================================
; helpers, real mode
; =============================================================================
puts_rm:
        push    ax
        push    si
.next:  lodsb
        or      al, al
        jz      .done
        call    putc_rm
        jmp     .next
.done:  pop     si
        pop     ax
        ret

putc_rm:
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     dx
        pop     ax
        ret

crlf_rm:
        push    ax
        mov     al, 13
        call    putc_rm
        mov     al, 10
        call    putc_rm
        pop     ax
        ret

put_dec_rm:                                     ; AL
        push    ax
        push    cx
        push    dx
        xor     ah, ah
        mov     cl, 10
        div     cl                              ; AL = tens, AH = units
        or      al, al
        jz      .units
        add     al, '0'
        call    putc_rm
.units: mov     al, ah
        add     al, '0'
        call    putc_rm
        pop     dx
        pop     cx
        pop     ax
        ret

put_hex16_rm:
        push    ax
        push    cx
        mov     cx, 4
.digit: rol     ax, 4
        push    ax
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    putc_rm
        pop     ax
        loop    .digit
        pop     cx
        pop     ax
        ret

; =============================================================================
msg_head:       db "DPMITEST: a DPMI host, as a 32-bit client sees it", 13, 10, 13, 10, 0
msg_none:       db "No DPMI host answered INT 2Fh AX=1687h.", 13, 10, 0
msg_present:    db "A host answered: DPMI ", 0
msg_flags:      db ", flags ", 0
msg_no32:       db "It does not take 32-bit clients.", 13, 10, 0
msg_refused:    db "The host refused the client.", 13, 10, 0
msg_rm_alloc:   db "(INT 21h 48h fails in real mode too)", 13, 10, 0
msg_in_pm:      db "In protected mode: CS DS SS ES = ", 0
msg_psp:        db "  the PSP through ES            ", 0
msg_version:    db "  INT 31h 0400h: version ", 0
msg_version2:   db ", flags ", 0
msg_desc:       db "  a descriptor onto the screen  ", 0
msg_mem:        db "  1 MB of extended memory       ", 0
msg_free:       db "  free memory                   ", 0
msg_kb:         db " KB", 13, 10, 0
msg_dos:        db "  4 KB of DOS memory            ", 0
msg_rmint:      db "  INT 21h 30h in real mode: AX=", 0
msg_rmint2:     db 13, 10, 0
msg_rmint3:     db "  a real-mode interrupt         ", 0
msg_exc:        db "  an exception handler          ", 0
msg_irq:        db "  the timer, hooked and chained ", 0
msg_cb:         db "  a real-mode callback          ", 0
msg_vif:        db "  the virtual interrupt flag    ", 0
msg_bye:        db "Leaving through INT 21h 4Ch.", 13, 10, 0
msg_ok:         db "ok", 13, 10, 0
msg_fail:       db "FAILED  AX=", 0
msg_crlf:       db 13, 10, 0

rm_seg:         dw 0
entry:          dd 0
host_paras:     dw 0
host_flags:     dw 0
host_ver:       dw 0
pm_ds:          dw 0
pm_cs:          dw 0
pm_ss:          dw 0
pm_es:          dw 0
sel_scr:        dw 0
sel_mem:        dw 0
mem_handle:     dd 0
mem_base:       dd 0
dos_sel:        dw 0
dos_seg:        dw 0
exc_hit:        db 0
exc_old:        dd 0
                dw 0
old8:           dd 0
                dw 0
ticks:          dw 0
cb_addr:        dd 0
cb_hit:         db 0
failures:       db 0
char_buf:       db 0, 0
                align 4
rmcs:           times 50 db 0
cb_rmcs:        times 50 db 0
mem_info:       times 48 db 0
out_buf:        times 256 db 0
