; =============================================================================
;  dpmi_io.inc - ports a client thinks it has, answered by a host that does not
; -----------------------------------------------------------------------------
;  A client runs at ring 3 with no privilege to touch ports, and the task
;  state segment's permission map decides which ones it may reach anyway.
;  Every bit left clear is a port the client uses directly - the timer, the
;  keyboard, the display - and every bit set is one the processor refuses,
;  raising a general protection fault the host then answers on the hardware's
;  behalf.
;
;  That fault is the whole trick.  It arrives *before* the instruction runs,
;  so a read can be given a value that no chip supplied; the debug registers
;  the speaker bridge uses trap afterwards, when the processor has already
;  latched whatever an empty port returned.  It is the difference between
;  impersonating a sound card and merely watching one being programmed.
;
;  What is here now is enough for a program to find the card and to send the
;  synthesiser its notes.  The digital side - the DSP's commands and the
;  direct memory access that feeds it samples - comes later.
; =============================================================================

; ---- the ports we answer for -------------------------------------------------
IO_ADLIB        equ 0x388               ; the synthesiser, and 0x389 beside it
IO_SB_BASE      equ 0x220               ; the card: 220h through 22Fh
IO_SB_TOP       equ 0x22F

[BITS 16]
; -----------------------------------------------------------------------------
; io_trap_port: set the bit for the port in DX, so the processor refuses it
; -----------------------------------------------------------------------------
io_trap_port:
        push    ax
        push    bx
        push    cx
        mov     bx, dx
        mov     cl, bl
        and     cl, 7
        shr     bx, 3
        cmp     bx, IOPB_BYTES
        jae     .out                    ; beyond the map: already refused
        mov     al, 1
        shl     al, cl
        or      [tss + TSS_IOPB + bx], al
.out:   pop     cx
        pop     bx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; io_trap_setup: the ports a Sound Blaster would answer to
; -----------------------------------------------------------------------------
io_trap_setup:
        push    dx
        mov     dx, IO_ADLIB
        call    io_trap_port
        inc     dx
        call    io_trap_port
        mov     dx, IO_SB_BASE
.sb:    call    io_trap_port
        inc     dx
        cmp     dx, IO_SB_TOP
        jbe     .sb
        pop     dx
        ret

[BITS 32]
; =============================================================================
;  The card itself, so far as a program can tell from outside
; =============================================================================
; -----------------------------------------------------------------------------
; io_dev_out: DX = port, EAX = the value, CL = 1, 2 or 4 bytes
; -----------------------------------------------------------------------------
io_dev_out:
        push    ebx
        inc     dword [io_writes]
        ; ---- the synthesiser: an address port and a data port beside it ----
        cmp     dx, 0x388
        je      .fm_addr
        cmp     dx, 0x389
        je      .fm_data
        cmp     dx, 0x220
        je      .fm_addr                ; the card answers for it here too
        cmp     dx, 0x221
        je      .fm_data
        cmp     dx, 0x222
        je      .fm_addr2               ; and the second bank
        cmp     dx, 0x223
        je      .fm_data2
        cmp     dx, 0x226
        je      .dsp_reset
        cmp     dx, 0x22C
        je      .dsp_write
        jmp     .done                   ; the mixer and the rest: swallowed
.fm_addr:
        mov     [opl_index], al
        mov     byte [opl_bank], 0
        jmp     .done
.fm_addr2:
        mov     [opl_index], al
        mov     byte [opl_bank], 1
        jmp     .done
.fm_data2:
.fm_data:
        movzx   ebx, byte [opl_index]
        cmp     byte [opl_bank], 0
        je      .store
        add     ebx, 256
.store: mov     [opl_regs + ebx], al
        inc     dword [opl_writes]
        jmp     .done
; ---- the DSP's reset: 1 then 0 means "are you there?", answered with AAh ----
.dsp_reset:
        test    al, 1
        jz      .reset_done
        mov     byte [dsp_resetting], 1
        jmp     .done
.reset_done:
        cmp     byte [dsp_resetting], 0
        je      .done
        mov     byte [dsp_resetting], 0
        mov     byte [dsp_reply], 0xAA
        mov     byte [dsp_has_reply], 1
        jmp     .done
; ---- a command; only the version is answered so far ----
.dsp_write:
        cmp     byte [dsp_expect], 0
        je      .cmd
        dec     byte [dsp_expect]       ; a parameter to a command we ignore
        jmp     .done
.cmd:   mov     [dsp_last], al
        cmp     al, 0xE1                ; what version are you?
        jne     .not_version
        mov     byte [dsp_reply], 4     ; a Sound Blaster Pro's answer
        mov     byte [dsp_has_reply], 1
        mov     byte [dsp_reply2], 5
        mov     byte [dsp_has_reply2], 1
        jmp     .done
.not_version:
        cmp     al, 0xE0                ; the identity echo: invert what follows
        je      .expect1
        cmp     al, 0x10                ; a sample straight to the speaker
        je      .expect1
        cmp     al, 0x40                ; the time constant
        je      .expect1
        jmp     .done
.expect1:
        mov     byte [dsp_expect], 1
.done:  pop     ebx
        ret

; -----------------------------------------------------------------------------
; io_dev_in: DX = port, CL = width -> EAX = what the card would have said
; -----------------------------------------------------------------------------
io_dev_in:
        push    ebx
        inc     dword [io_reads]
        xor     eax, eax
        cmp     dx, 0x388
        je      .fm_status
        cmp     dx, 0x220
        je      .fm_status
        cmp     dx, 0x22A
        je      .dsp_read
        cmp     dx, 0x22C
        je      .dsp_write_status
        cmp     dx, 0x22E
        je      .dsp_read_status
        mov     eax, 0xFF               ; an unclaimed port reads as nothing
        pop     ebx
        ret
; The synthesiser's status: the timers, which a program uses to decide the
; chip is really there.  Both bits set once a timer has been started and
; expired, which as far as anyone outside can tell is immediately.
.fm_status:
        mov     al, [opl_regs + 4]      ; the timer control register
        test    al, 0x80                ; a reset of the flags
        jnz     .fm_clear
        mov     al, [opl_timer_flags]
        pop     ebx
        ret
.fm_clear:
        mov     byte [opl_timer_flags], 0
        xor     eax, eax
        pop     ebx
        ret
.dsp_read:
        mov     al, [dsp_reply]
        cmp     byte [dsp_has_reply], 0
        je      .no_reply
        mov     byte [dsp_has_reply], 0
        movzx   ebx, byte [dsp_has_reply2]
        or      bl, bl
        jz      .ret_reply
        mov     bl, [dsp_reply2]        ; the second half moves up
        mov     [dsp_reply], bl
        mov     byte [dsp_has_reply], 1
        mov     byte [dsp_has_reply2], 0
.ret_reply:
        movzx   eax, al
        pop     ebx
        ret
.no_reply:
        mov     eax, 0xFF
        pop     ebx
        ret
.dsp_write_status:                      ; bit 7 clear: ready for a command
        xor     eax, eax
        pop     ebx
        ret
.dsp_read_status:                       ; bit 7 set: there is something to read
        mov     al, [dsp_has_reply]
        or      al, al
        jz      .nothing
        mov     eax, 0x80
        pop     ebx
        ret
.nothing:
        xor     eax, eax
        pop     ebx
        ret

[BITS 16]
; ---- what the card remembers ------------------------------------------------
io_writes:      dd 0                    ; counted so a test can see them arrive
io_reads:       dd 0
opl_writes:     dd 0
opl_index:      db 0
opl_bank:       db 0
opl_timer_flags: db 0xC0                ; both timers, as an idle chip reports
dsp_resetting:  db 0
dsp_has_reply:  db 0
dsp_reply:      db 0
dsp_has_reply2: db 0
dsp_reply2:     db 0
dsp_expect:     db 0
dsp_last:       db 0
                align 4
opl_regs:       times 512 db 0
