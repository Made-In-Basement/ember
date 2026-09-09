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
        push    cx
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
        ; The card does not fetch its own samples: the transfer controller
        ; does, and a game programs that instead.  Watching the card without
        ; watching this would be watching somebody dial a telephone and
        ; never hearing the call.
        xor     dx, dx
.dma:   call    io_trap_port                    ; 00h-0Fh: the four channels
        inc     dx
        cmp     dx, 0x0F
        jbe     .dma
        mov     cx, 3
        mov     dx, 0x81                        ; 81h-83h, and 87h below
.page:  call    io_trap_port
        inc     dx
        loop    .page
        mov     dx, 0x87
        call    io_trap_port
        pop     dx
        pop     cx
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
        cmp     dx, 0x10
        jb      .dma_chan
        cmp     dx, 0x81
        je      .dma_page
        cmp     dx, 0x82
        je      .dma_page
        cmp     dx, 0x83
        je      .dma_page
        cmp     dx, 0x87
        je      .dma_page
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
        dec     byte [dsp_expect]
        movzx   ebx, byte [dsp_param_n]
        and     ebx, 3
        mov     [dsp_param + ebx], al
        inc     byte [dsp_param_n]
        cmp     byte [dsp_expect], 0
        jne     .done                   ; more of them to come
        ; the last one has arrived; act on the command it belonged to
        mov     al, [dsp_last]
        cmp     al, 0x40
        je      .set_rate
        cmp     al, 0x48
        je      .set_len
        cmp     al, 0x14
        je      .set_len_and_go
        jmp     .done
.set_rate:
        mov     al, [dsp_param]         ; 256 - 1000000/rate, as given
        mov     [sb_tc], al
        jmp     .done
.set_len:
        mov     al, [dsp_param]
        mov     [sb_len], al
        mov     al, [dsp_param + 1]
        mov     [sb_len + 1], al
        jmp     .done
.set_len_and_go:
        mov     al, [dsp_param]
        mov     [sb_len], al
        mov     al, [dsp_param + 1]
        mov     [sb_len + 1], al
        mov     byte [sb_auto], 0
        jmp     .started
.cmd:   mov     [dsp_last], al
        cmp     al, 0x40                ; the time constant: one byte, not two
        je      .expect1
        cmp     al, 0x48                ; how long a block is
        je      .expect2
        cmp     al, 0x14                ; play a block, once
        je      .expect2
        cmp     al, 0x1C                ; play, and keep playing
        je      .start_auto
        cmp     al, 0xD0                ; hold
        je      .pause
        cmp     al, 0xD4                ; carry on
        je      .resume
        cmp     al, 0xD1                ; the speaker on
        je      .spk_on
        cmp     al, 0xD3                ; and off
        je      .spk_off
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
        jmp     .done
.expect2:
        mov     byte [dsp_expect], 2
        mov     byte [dsp_param_n], 0
        jmp     .done
.start_auto:
        mov     byte [sb_auto], 1
        jmp     .started
.pause: mov     byte [sb_playing], 0
        jmp     .done
.resume:
        cmp     word [sb_len], 0
        je      .done
        mov     byte [sb_playing], 1
        jmp     .done
.spk_on:
        mov     byte [sb_speaker], 1
        jmp     .done
.spk_off:
        mov     byte [sb_speaker], 0
        jmp     .done
.started:
        mov     byte [sb_playing], 1
        inc     dword [sb_starts]
        jmp     .done
.expect1:
        mov     byte [dsp_expect], 1
        mov     byte [dsp_param_n], 0
        jmp     .done

; ---- the transfer controller, 00h-0Fh ---------------------------------------
; Addresses and counts arrive a byte at a time, low then high, and which half
; comes next is remembered in a flip-flop that port 0Ch resets.  Get that
; wrong and every buffer address is half right, which is worse than wrong.
.dma_chan:
        mov     ebx, edx
        cmp     bl, 0x08
        jae     .dma_ctrl
        shr     ebx, 1                          ; the channel it belongs to
        test    dl, 1
        jnz     .dma_count
        cmp     byte [dma_ff], 0
        jne     .dma_addr_hi
        mov     [dma_addr + ebx*2], al
        mov     byte [dma_ff], 1
        jmp     .done
.dma_addr_hi:
        mov     [dma_addr + ebx*2 + 1], al
        mov     byte [dma_ff], 0
        jmp     .done
.dma_count:
        cmp     byte [dma_ff], 0
        jne     .dma_count_hi
        mov     [dma_count + ebx*2], al
        mov     byte [dma_ff], 1
        jmp     .done
.dma_count_hi:
        mov     [dma_count + ebx*2 + 1], al
        mov     byte [dma_ff], 0
        jmp     .done
.dma_ctrl:
        cmp     bl, 0x0C                        ; clear the flip-flop
        je      .dma_clear
        cmp     bl, 0x0B                        ; the mode register
        je      .dma_mode
        cmp     bl, 0x0A                        ; mask one channel
        je      .dma_mask
        jmp     .done
.dma_clear:
        mov     byte [dma_ff], 0
        jmp     .done
.dma_mode:
        movzx   ebx, al
        and     ebx, 3
        mov     [dma_mode + ebx], al
        jmp     .done
.dma_mask:
        mov     [dma_mask], al
        jmp     .done
; The page registers are not in channel order, because nothing about this
; controller is: 87h is channel 0, 83h channel 1, 81h channel 2, 82h three.
.dma_page:
        cmp     dl, 0x87
        jne     .page_81
        xor     ebx, ebx
        jmp     .page_have
.page_81:
        cmp     dl, 0x81
        jne     .page_82
        mov     ebx, 2
        jmp     .page_have
.page_82:
        cmp     dl, 0x82
        jne     .page_83
        mov     ebx, 3
        jmp     .page_have
.page_83:
        mov     ebx, 1
.page_have:
        mov     [dma_page + ebx], al
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
; -----------------------------------------------------------------------------
; io_report: what the card was told, once the client that told it has gone.
;   Nothing is written unless a client touched the card at all, so a program
;   that wanted no sound leaves no trace.
; -----------------------------------------------------------------------------
; io_say: one line, to the screen and to the log.  DS:SI text, EAX value.
;   The log is only written out at certain moments and a client leaving is
;   not one of them, so the screen is what a test can actually read.
io_say:
        push    si
        push    eax
        SVC     SVC_LOG_LINE
        pop     eax
        pop     si
        push    eax
        SVC     SVC_PUTS
        pop     eax
        push    eax
        shr     eax, 16
        SVC     SVC_PRINT_HEX16
        pop     eax
        SVC     SVC_PRINT_HEX16
        SVC     SVC_CRLF
        ret

io_report:
        cmp     dword [io_writes], 0
        jne     .say
        cmp     dword [io_reads], 0
        je      .none
.say:   mov     si, msg_io_w
        mov     eax, [io_writes]
        call    io_say
        mov     si, msg_io_r
        mov     eax, [io_reads]
        call    io_say
        cmp     dword [opl_writes], 0
        je      .no_fm
        mov     si, msg_io_fm
        mov     eax, [opl_writes]
        call    io_say
.no_fm:
        cmp     dword [sb_starts], 0
        je      .none
        mov     si, msg_io_go
        mov     eax, [sb_starts]
        call    io_say
        ; where the samples are: the page register carries the top bits the
        ; address register has no room for
        movzx   eax, byte [dma_page + 1]
        shl     eax, 16
        movzx   ebx, word [dma_addr + 2]
        or      eax, ebx
        mov     si, msg_io_buf
        call    io_say
        movzx   eax, word [dma_count + 2]
        mov     si, msg_io_cnt
        call    io_say
        movzx   eax, word [sb_len]
        mov     si, msg_io_len
        call    io_say
        movzx   eax, byte [sb_tc]
        mov     si, msg_io_tc
        call    io_say
        movzx   eax, byte [sb_auto]
        mov     si, msg_io_auto
        call    io_say
.none:  ret

msg_io_w:       db "SB: writes to the card       ", 0
msg_io_r:       db "SB: reads from it            ", 0
msg_io_fm:      db "SB: writes to the synthesiser", 0
msg_io_go:      db "SB: transfers begun          ", 0
msg_io_buf:     db "SB: the buffer at            ", 0
msg_io_cnt:     db "SB: the controller's count   ", 0
msg_io_len:     db "SB: the block the DSP wants  ", 0
msg_io_tc:      db "SB: the time constant        ", 0
msg_io_auto:    db "SB: keeps playing by itself  ", 0

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
dsp_param_n:    db 0
dsp_param:      times 4 db 0
sb_tc:          db 0                    ; the time constant, as the game gave it
sb_len:         dw 0                    ; a block, one less than its length
sb_auto:        db 0                    ; keeps going without being asked again
sb_playing:     db 0
sb_speaker:     db 0
                align 4
sb_starts:      dd 0                    ; transfers begun
dma_ff:         db 0                    ; which half of an address comes next
dma_mask:       db 0
                align 2
dma_addr:       times 4 dw 0
dma_count:      times 4 dw 0
dma_page:       times 4 db 0
dma_mode:       times 4 db 0
                align 4
opl_regs:       times 512 db 0
