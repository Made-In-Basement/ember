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
IO_ESCAPE       equ 0x2FF               ; not a port: how sb.asm leaves V86
IO_PIC          equ 0x20                ; the interrupt controller, and its mask
IO_PIT2         equ 0x42                ; the timer channel the speaker is on
IO_PITCMD       equ 0x43                ; ...and the command register beside it
IO_GATE         equ 0x61                ; the gate that lets it through
SPK_AMPL        equ 6000                ; how loud a square wave is here
PIT_HZ          equ 1193182

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
        ;
        ; Only channel 1's own registers, though - its address, its count and
        ; the page above them - because the other three channels belong to
        ; whatever else is in the machine, and a floppy that cannot reach
        ; memory is a poor trade for a sound card.  The three registers every
        ; channel shares are watched and then let through to the real
        ; controller, so the shadow here and the hardware agree.
        mov     dx, 0x02                        ; channel 1: the address
        call    io_trap_port
        inc     dx                              ; and how many bytes
        call    io_trap_port
        mov     dx, 0x0A                        ; the mask
        call    io_trap_port
        inc     dx                              ; the mode
        call    io_trap_port
        inc     dx                              ; and the flip-flop's reset
        call    io_trap_port
        mov     dx, 0x83                        ; channel 1's page
        call    io_trap_port
        mov     dx, IO_ESCAPE
        call    io_trap_port
        pop     dx
        pop     cx
        ret

; -----------------------------------------------------------------------------
; io_trap_pic: the interrupt controller as well.  Only the V86 monitor asks
;   for this: it needs the card's own interrupt to look real, which means
;   knowing what the program has unmasked and swallowing the end-of-interrupt
;   the program sends for an interrupt no chip ever raised.
; -----------------------------------------------------------------------------
; -----------------------------------------------------------------------------
; io_trap_speaker: the timer channel the speaker hangs off, and its gate.
;   Only the V86 monitor asks for this.  Watching them here is strictly better
;   than the debug registers spkbrdg.asm uses: the fault arrives before the
;   instruction rather than after, and the processor's four breakpoints stay
;   free for whoever wants them.
; -----------------------------------------------------------------------------
io_trap_speaker:
        push    dx
        mov     dx, IO_PIT2
        call    io_trap_port
        inc     dx
        call    io_trap_port
        mov     dx, IO_GATE
        call    io_trap_port
        pop     dx
        ret

io_trap_pic:
        push    dx
        mov     dx, IO_PIC
        call    io_trap_port
        inc     dx
        call    io_trap_port
        pop     dx
        ret

; -----------------------------------------------------------------------------
; io_card_reset: forget the last program's card.  Counters and all: each run
;   reports its own.
; -----------------------------------------------------------------------------
io_card_reset:
        push    ax
        push    cx
        push    di
        push    es
        push    cs
        pop     es
        mov     di, io_state
        mov     cx, (io_state_end - io_state) / 2
        xor     ax, ax
        rep     stosw
        mov     byte [opl_timer_flags], 0xC0
        mov     byte [pic_mask], 0xFF
        pop     es
        pop     di
        pop     cx
        pop     ax
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
        cmp     dx, IO_ESCAPE
        je      .escape
        cmp     dx, 0x20
        je      .pic_cmd
        cmp     dx, 0x21
        je      .pic_mask
        cmp     dx, IO_PITCMD
        je      .pit_cmd
        cmp     dx, IO_PIT2
        je      .pit_data
        cmp     dx, IO_GATE
        je      .gate
        cmp     dx, 0x224
        je      .mix_addr
        cmp     dx, 0x225
        je      .mix_data
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
        cmp     dx, 0x400
        jae     .passthru               ; not in the map at all: the machine's
        jmp     .done                   ; the mixer and the rest: swallowed
; The permission map only reaches port 3FFh; everything above it faults
; whether this host wants it or not, so what a program meant by it is done
; for real.  Ring 0 may touch any port, map or no map.
.passthru:
        cmp     cl, 1
        je      .pass1
        cmp     cl, 2
        je      .pass2
        out     dx, eax
        jmp     .done
.pass1: out     dx, al
        jmp     .done
.pass2: out     dx, ax
        jmp     .done
; ---- not a port at all: the monitor's way of asking to leave V86 mode -------
.escape:
        mov     byte [io_leave], 1
        jmp     .done
; ---- the mixer a Pro has, which is part of how a Pro is recognised ---------
; Writing register 0 resets it; everything else is remembered and reads back,
; which is the test a program makes of it.
.mix_addr:
        mov     [mix_index], al
        jmp     .done
.mix_data:
        movzx   ebx, byte [mix_index]
        or      bl, bl
        jz      .mix_reset
        mov     [mix_regs + ebx], al
        jmp     .done
.mix_reset:
        push    edi
        push    ecx
        push    eax
        mov     edi, mix_regs
        mov     ecx, 256
        xor     eax, eax
        mov     al, 0xBB                ; what a Pro comes up with: middling
        rep     stosb
        pop     eax
        pop     ecx
        pop     edi
        jmp     .done

; ---- the speaker -----------------------------------------------------------
; A program makes a note by programming timer channel 2 and opening the gate
; in port 61h.  On this machine that signal goes nowhere: there is no beeper,
; and the chipset does not feed the line into the audio codec.  So what the
; timer was told is remembered here, and io_pump lays the square wave it
; describes into the same stream the card's samples go to.
;
; Every write is made for real as well.  Channels 0 and 1 of that timer are
; the machine's own clock and nothing here may take them away, and writing
; channel 2 on a machine with no speaker attached to it costs nothing.
.pit_cmd:
        out     IO_PITCMD, al
        mov     ebx, eax
        and     bl, 0xC0
        cmp     bl, 0x80                ; is it channel 2 being spoken to?
        jne     .done
        mov     ebx, eax
        shr     bl, 4
        and     bl, 3
        mov     [pcs_access], bl        ; 1 = low byte, 2 = high, 3 = both
        mov     ebx, eax
        shr     bl, 1
        and     bl, 7
        mov     [pcs_mode], bl
        mov     byte [pcs_toggle], 0
        jmp     .done
.pit_data:
        out     IO_PIT2, al
        cmp     byte [pcs_access], 3
        jne     .pit_one
        cmp     byte [pcs_toggle], 0
        jne     .pit_high
        mov     [pcs_reload], al
        mov     byte [pcs_toggle], 1
        jmp     .pit_tuned
.pit_high:
        mov     [pcs_reload + 1], al
        mov     byte [pcs_toggle], 0
        jmp     .pit_tuned
.pit_one:
        cmp     byte [pcs_access], 2
        je      .pit_only_high
        mov     [pcs_reload], al
        mov     byte [pcs_reload + 1], 0
        jmp     .pit_tuned
.pit_only_high:
        mov     [pcs_reload + 1], al
        mov     byte [pcs_reload], 0
.pit_tuned:
        call    pcs_retune
        jmp     .done
.gate:
        out     IO_GATE, al
        and     al, 3                   ; the gate, and the speaker behind it
        mov     [pcs_gate], al
        call    pcs_retune
        jmp     .done

; ---- the interrupt controller ----------------------------------------------
; The card's interrupt is one this host raises itself, so the controller never
; has it in service and the end-of-interrupt a program sends for it would
; clear whatever really is - the timer, most likely, in whose handler the
; card's own is nested.  So that one is swallowed and the rest let through.
.pic_cmd:
        cmp     byte [pic_isr], 0
        je      .pic_through
        mov     byte [pic_isr], 0
        jmp     .done
.pic_through:
        out     0x20, al
        jmp     .done
; A program that unmasks the card's line unmasks nothing: no chip is on it.
; What it wanted is remembered, so this host knows whether to raise anything.
.pic_mask:
        mov     [pic_mask], al
        or      al, 0x20                ; line 5 stays masked in the hardware
        out     0x21, al
        jmp     .done
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
%ifdef HAVE_OPL
        push    eax                             ; ...and to the chip itself
        push    edx
        push    ecx
        movzx   eax, al
        push    eax
        push    ebx
        call    OPL_WRITE
        add     esp, 8
        pop     ecx
        pop     edx
        pop     eax
%endif
        cmp     bl, 4                   ; the timer control register
        jne     .done
        cmp     byte [opl_bank], 0
        jne     .done
        ; A program finds a synthesiser by resetting the timer flags, reading
        ; the status (nothing set), starting a timer, waiting, and reading
        ; again (both set).  There is no time here to wait through, so a timer
        ; that has been started has already expired.
        test    al, 0x80                ; bit 7 resets the flags
        jz      .timer_run
        mov     byte [opl_timer_flags], 0
        jmp     .done
.timer_run:
        mov     byte [opl_timer_flags], 0xC0
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
        ; the buffer is whatever the transfer controller was last told about
        ; channel 1; the page register holds the bits the address cannot
        movzx   ebx, byte [dma_page + 1]
        shl     ebx, 16
        movzx   eax, word [dma_addr + 2]
        or      ebx, eax
        mov     [au_src], ebx
        mov     dword [au_spos], 0
        mov     byte [au_primed], 0             ; place the write again
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
        ; All three belong to every channel at once, so the write is noted
        ; and then made for real: the other three channels are somebody
        ; else's and must go on working.
.dma_clear:
        mov     byte [dma_ff], 0
        out     0x0C, al
        jmp     .done
.dma_mode:
        movzx   ebx, al
        and     ebx, 3
        mov     [dma_mode + ebx], al
        out     0x0B, al
        jmp     .done
.dma_mask:
        mov     [dma_mask], al
        out     0x0A, al
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
; pcs_retune: what the timer has been told, turned into a step through a
;   square wave.  Done once whenever something changes rather than per sample.
;   Modes 2 and 3 (and 6 and 7, which are the same two again) make a tone; the
;   others are one-shots a program uses for timing, not for sound.
; -----------------------------------------------------------------------------
pcs_retune:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     byte [pcs_on], 0
        cmp     byte [pcs_gate], 3      ; the gate open and the speaker on
        jne     .silent
        mov     al, [pcs_mode]
        and     al, 3
        cmp     al, 2
        je      .tone
        cmp     al, 3
        jne     .silent
.tone:
        movzx   ecx, word [pcs_reload]
        or      ecx, ecx
        jnz     .have_reload
        mov     ecx, 65536              ; nought means the whole count
.have_reload:
        cmp     ecx, 20                 ; above 59 kHz: nothing anyone hears
        jb      .silent
        mov     eax, [au_rate]
        or      eax, eax
        jz      .silent
        mul     ecx                     ; the rate times the count
        or      edx, edx
        jnz     .silent
        mov     ecx, eax
        mov     eax, (PIT_HZ << 16) & 0xFFFFFFFF
        mov     edx, PIT_HZ >> 16
        div     ecx                     ; the step, in sixteen-sixteen
        mov     [pcs_step], eax
        mov     byte [pcs_on], 1
.silent:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; -----------------------------------------------------------------------------
; io_dev_in: DX = port, CL = width -> EAX = what the card would have said
; -----------------------------------------------------------------------------
io_dev_in:
        push    ebx
        inc     dword [io_reads]
        xor     eax, eax
        cmp     dx, 0x21
        je      .pic_mask               ; what the program last asked for
        cmp     dx, 0x20
        je      .pic_read               ; whatever the real one says
        cmp     dx, IO_GATE
        je      .gate_read              ; the refresh bit some programs time by
        cmp     dx, IO_PIT2
        je      .pit_read
        cmp     dx, 0x388
        je      .fm_status
        cmp     dx, 0x220
        je      .fm_status
        cmp     dx, 0x225
        je      .mix_read
        cmp     dx, 0x22A
        je      .dsp_read
        cmp     dx, 0x22C
        je      .dsp_write_status
        cmp     dx, 0x22E
        je      .dsp_ack
        cmp     dx, 0x400
        jae     .passthru               ; above the map: really read it
        mov     eax, 0xFF               ; an unclaimed port reads as nothing
        pop     ebx
        ret
.passthru:
        cmp     cl, 1
        je      .pass1
        cmp     cl, 2
        je      .pass2
        in      eax, dx
        pop     ebx
        ret
.pass1: in      al, dx
        movzx   eax, al
        pop     ebx
        ret
.pass2: in      ax, dx
        movzx   eax, ax
        pop     ebx
        ret
; A program that means to unmask its own line reads the mask, clears a bit and
; writes it back.  Answering that read with nothing would tell it every line
; is masked, and it would then mask every line - including the timer, and the
; keyboard, and itself.
.pic_mask:
        movzx   eax, byte [pic_mask]
        pop     ebx
        ret
.pic_read:
        in      al, 0x20
        movzx   eax, al
        pop     ebx
        ret
.gate_read:
        in      al, IO_GATE
        movzx   eax, al
        pop     ebx
        ret
.pit_read:
        in      al, IO_PIT2
        movzx   eax, al
        pop     ebx
        ret

; The synthesiser's status: the timer flags, kept by the writes to register
; 4 rather than worked out here, so that a program reading twice in a row
; gets the same answer both times.
.fm_status:
        movzx   eax, byte [opl_timer_flags]
        pop     ebx
        ret
.mix_read:
        movzx   ebx, byte [mix_index]
        movzx   eax, byte [mix_regs + ebx]
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
; Reading this is how a program's interrupt handler tells the card it has
; been heard, as well as how it asks whether there is anything to read.
.dsp_ack:
        mov     byte [sb_card_irq], 0
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

; =============================================================================
;  io_pump - the game's samples into the machine's own stream
; -----------------------------------------------------------------------------
;  A Sound Blaster is fed by the transfer controller a byte at a time at
;  whatever rate its time constant asks for; this machine's audio hardware
;  wants pairs of sixteen-bit samples at 44100 a second and never stops
;  asking.  So the two are joined here: walk the game's buffer at its own
;  rate, in sixteen-sixteen fixed point, and lay each byte down as many times
;  as 44100 requires.
;
;  Nothing here waits.  It runs on the timer interrupt, before the client's
;  own handler sees it, and fills only as far ahead of the hardware as it is
;  allowed to be.
; =============================================================================
[BITS 32]
AU_LEAD         equ 8192                        ; frames kept ahead of the DMA

io_pump:
        inc     dword [au_calls]
        cmp     byte [au_have], 0
        je      .out
        inc     dword [au_ready]
        pushad
        ; What is making a sound?  Either, both, or - once something has -
        ; neither, which still has to be written, because the ring plays on
        ; whatever is in it and silence has to be put there.
        ; A transfer runs whether or not the speaker is on: the speaker only
        ; decides whether anyone hears it, and a program that never turns it
        ; on still expects to be told its block has been played.  That is how
        ; some of them decide the card is there at all.
        mov     al, [sb_playing]
        mov     [pump_dsp], al
.no_dsp:
        mov     al, [pump_dsp]
        or      al, [pcs_on]
%ifdef HAVE_OPL
        or      al, [OPL_LIVE]                  ; the music counts as well
%endif
        jnz     .work
        cmp     byte [au_primed], 0
        je      .done                           ; nothing has ever played
.work:
        ; how far the hardware has got, in frames
        mov     esi, [au_lpib]
        mov     eax, [gs:esi]
        shr     eax, 2
        mov     ecx, [au_bytes]
        shr     ecx, 2                          ; the ring, in frames
        or      ecx, ecx
        jz      .done
        xor     edx, edx
        div     ecx
        mov     ebx, edx                        ; where it is playing

        ; Where to write first.  In a ring, so many frames ahead and the same
        ; number behind are the one number, so the first write has to be
        ; placed deliberately rather than left at zero and reasoned about.
        cmp     byte [au_primed], 0
        jne     .placed
        mov     eax, ebx
        add     eax, 1024                       ; just in front of the hardware
        xor     edx, edx
        div     ecx
        mov     [au_wpos], edx
        mov     byte [au_primed], 1
.placed:
        ; how far ahead we already are
        mov     eax, [au_wpos]
        sub     eax, ebx
        add     eax, ecx
        xor     edx, edx
        div     ecx
        mov     eax, edx                        ; frames in hand
        cmp     eax, AU_LEAD
        jae     .done                           ; enough for now
        mov     edi, AU_LEAD
        sub     eax, eax
        mov     eax, AU_LEAD
        sub     eax, edx
        mov     edi, eax                        ; frames to lay down
        add     [au_frames], edi

        ; The step through the game's buffer, in sixteen-sixteen.  The card is
        ; told a time constant rather than a rate: 256 - 1000000/rate.
        cmp     byte [pump_dsp], 0
        je      .fill
        movzx   ebx, byte [sb_tc]
        mov     ecx, 256
        sub     ecx, ebx
        jle     .mute_dsp                       ; a constant of 256 says nothing
        mov     eax, 1000000
        xor     edx, edx
        div     ecx                             ; EAX = the rate it wants
        shl     eax, 16
        xor     edx, edx
        mov     ecx, [au_rate]
        or      ecx, ecx
        jz      .mute_dsp
        div     ecx
        mov     [au_step], eax
        jmp     .fill
.mute_dsp:
        mov     byte [pump_dsp], 0

.fill:
        xor     ebx, ebx                        ; the frame, mixed
        cmp     byte [pump_dsp], 0
        je      .no_sample
        ; the sample the game has got to
        mov     eax, [au_spos]
        shr     eax, 16
        movzx   ecx, word [sb_len]
        inc     ecx                             ; the block's length
        cmp     eax, ecx
        jb      .have_sample
        cmp     byte [sb_auto], 0
        jne     .rewind
        mov     byte [sb_playing], 0            ; one pass, and it is over
        mov     byte [pump_dsp], 0
        inc     dword [sb_blocks]
        mov     byte [sb_irq_owed], 1
        jmp     .no_sample                      ; the speaker plays on alone
.rewind:
        mov     dword [au_spos], 0
        inc     dword [sb_blocks]
        mov     byte [sb_irq_owed], 1
        xor     eax, eax
.have_sample:
        add     eax, [au_src]
        movzx   eax, byte [gs:eax]
        sub     eax, 128                        ; the card's silence is 80h
        shl     eax, 8                          ; and this stream's is zero
        cmp     byte [sb_speaker], 0            ; ...but muted if it is muted
        je      .no_sample
        mov     ebx, eax
.no_sample:
%ifdef HAVE_OPL
        ; and the synthesiser, which is the music in nearly every game that
        ; had any.  A frame at a time: eighteen voices of frequency modulation
        ; dwarf the cost of the call, and it saves keeping a buffer here.
        cmp     byte [OPL_LIVE], 0
        je      .no_fm
        call    OPL_RENDER1
        movsx   eax, word [OPL_FRAME]
        add     ebx, eax
.no_fm:
%endif
        ; and the square wave the timer describes, added to it
        cmp     byte [pcs_on], 0
        je      .no_square
        mov     eax, [pcs_phase]
        add     eax, [pcs_step]
        mov     [pcs_phase], eax
        test    eax, 0x8000                     ; half a cycle either way
        jz      .square_low
        add     ebx, SPK_AMPL
        jmp     .no_square
.square_low:
        sub     ebx, SPK_AMPL
.no_square:
        cmp     ebx, 32767
        jle     .not_loud
        mov     ebx, 32767
.not_loud:
        cmp     ebx, -32768
        jge     .not_quiet
        mov     ebx, -32768
.not_quiet:
        movzx   eax, bx
        mov     edx, eax
        shl     edx, 16
        or      eax, edx                        ; the same in both ears

        mov     ecx, [au_wpos]
        mov     esi, [au_ring]
        mov     [gs:esi + ecx*4], eax
        inc     ecx
        mov     edx, [au_bytes]
        shr     edx, 2
        cmp     ecx, edx
        jb      .no_wrap
        xor     ecx, ecx
.no_wrap:
        mov     [au_wpos], ecx

        cmp     byte [pump_dsp], 0
        je      .next
        mov     eax, [au_spos]
        add     eax, [au_step]
        mov     [au_spos], eax
.next:
        dec     edi
        jnz     .fill
.done:
        wbinvd                                  ; the controller reads memory
        popad
.out:   ret

[BITS 16]
; -----------------------------------------------------------------------------
; io_report: what the card was told, once the client that told it has gone.
;   Nothing is written unless a client touched the card at all, so a program
;   that wanted no sound leaves no trace.
; -----------------------------------------------------------------------------
; io_say: one line, to the screen and to the log.  DS:SI text, EAX value.
;   The log is only written out at certain moments and a client leaving is
;   not one of them, so the screen is what a test can actually read.
; -----------------------------------------------------------------------------
; io_audio_open: take the HD Audio stream for as long as a client runs.
;   Both ends of a client's life are real-mode moments, so the kernel's own
;   call will do and no excursion is needed.  A client that makes no sound
;   holds the stream and returns it unused, which costs nothing.
; -----------------------------------------------------------------------------
io_audio_open:
        push    ax
        push    dx
        mov     byte [au_have], 0
        mov     dx, au_info
        mov     ah, 0xF0
        int     0x21
        jc      .none
        mov     byte [au_have], 1
.none:  pop     dx
        pop     ax
        ret

io_audio_close:
        push    ax
        cmp     byte [au_have], 0
        je      .none
        mov     ah, 0xF1
        int     0x21
        mov     byte [au_have], 0
.none:  pop     ax
        ret

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
        cmp     byte [au_have], 0
        je      .no_audio
        mov     si, msg_io_ring
        mov     eax, [au_ring]
        call    io_say
        mov     si, msg_io_rate
        mov     eax, [au_rate]
        call    io_say
.no_audio:
        cmp     dword [sb_starts], 0
        je      .none
        mov     si, msg_io_calls
        mov     eax, [au_calls]
        call    io_say
        mov     si, msg_io_ready
        mov     eax, [au_ready]
        call    io_say
        mov     si, msg_io_frames
        mov     eax, [au_frames]
        call    io_say
        mov     si, msg_io_blk
        mov     eax, [sb_blocks]
        call    io_say
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

msg_io_calls:   db "SB: the pump was asked       ", 0
msg_io_ready:   db "SB: ...with a stream to fill ", 0
msg_io_frames:  db "SB: frames it laid down      ", 0
msg_io_blk:     db "SB: blocks played out        ", 0
msg_io_ring:    db "SB: the audio ring at        ", 0
msg_io_rate:    db "SB: the stream plays at      ", 0
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
; Everything from io_state to io_state_end is cleared between programs, so
; each run of a game meets a card that has just been switched on.
;
; The eight bytes in front of it are there to be searched for.  A program
; that hangs never reaches io_report, and the counters are the only thing
; that says what it was doing; tools/sbpeek.py finds this in a memory dump
; and reads them out.  Eight bytes is a small price for being able to ask a
; hung machine what happened.
io_probe:       db "SBCARD!!"
io_state:
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
; what the kernel hands back: the ring, its size, where the hardware says it
; has got to, and the rate it plays at
au_have:        db 0
                align 4
au_info:
au_ring:        dd 0
au_bytes:       dd 0
au_lpib:        dd 0
au_rate:        dd 0
au_wpos:        dd 0                    ; where we are writing, in frames
au_spos:        dd 0                    ; the game's buffer, 16.16
au_step:        dd 0
au_src:         dd 0                    ; the buffer, as the bus sees it
sb_blocks:      dd 0                    ; blocks played out
au_calls:       dd 0                    ; times the pump was asked to run
au_ready:       dd 0                    ; times it had a stream to fill
au_frames:      dd 0                    ; frames it laid down
au_primed:      db 0                    ; the write position has been placed
sb_irq_owed:    db 0                    ; a block is done; the card would say so
sb_card_irq:    db 0                    ; ...and has not been acknowledged yet
pic_isr:        db 0                    ; the card's interrupt is being served
pic_mask:       db 0xFF                 ; what the program asked the controller
io_leave:       db 0                    ; the monitor has been asked to stand down
pump_dsp:       db 0                    ; the card has samples this time round
pcs_on:         db 0                    ; the speaker has a note to make
pcs_mode:       db 0                    ; what the timer channel was told
pcs_access:     db 0
pcs_toggle:     db 0
pcs_gate:       db 0
                align 2
pcs_reload:     dw 0
                align 4
pcs_step:       dd 0                    ; through the wave, per frame, 16.16
pcs_phase:      dd 0
mix_index:      db 0
                align 4
mix_regs:       times 256 db 0
                align 4
opl_regs:       times 512 db 0
io_state_end:
