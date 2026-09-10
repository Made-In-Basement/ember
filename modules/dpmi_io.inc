%ifndef OPL_OWN_STACK                   ; the monitor's stack already agrees
%macro OPL_ENTER 0
%endmacro
%macro OPL_LEAVE 0
%endmacro
%endif

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
        ; A real card comes up with its output switched off and waits to be
        ; told D1h.  Nearly every program does tell it, and the ones that do
        ; not are relying on a mixer that was never off in the first place -
        ; so this one comes up on.  Silence because nobody said the magic
        ; word is the worse of the two ways to be wrong.
        mov     byte [sb_speaker], 1
        mov     eax, [au_lead_start]            ; nought would fill nothing
        mov     [au_lead], eax
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
        OPL_ENTER
        push    eax
        push    ebx
        call    OPL_WRITE
        add     esp, 8
        OPL_LEAVE
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
        ; The Pro's high-speed transfers.  They are the same transfers with
        ; the chip's own command handling switched off for the duration - it
        ; is how a Pro reaches 22 kHz and above - and there is nothing here
        ; that runs at a speed, so they are simply the ordinary ones.  DMX,
        ; which is the sound in Doom, Heretic, Hexen and Raptor, asks for 90h
        ; and nothing else.
        cmp     al, 0x90                ; high speed, and keep playing
        je      .start_auto
        cmp     al, 0x91                ; high speed, once
        je      .start_once
        cmp     al, 0xDA                ; leave auto-initialise
        je      .stop_auto
        cmp     al, 0xD8                ; is the speaker on?
        je      .speaker_status
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
        ; 3.02: a Sound Blaster Pro 2, which is honestly what this is - eight
        ; bits, one channel, transfer channel 1, auto-initialise, and the
        ; Pro's mixer.  It used to say 4.5, which is a Sound Blaster 16, and
        ; a game that believes that will ask for sixteen-bit stereo on a
        ; transfer channel nothing here answers to and get silence for it.
        ; Claiming less than is delivered is safe; claiming more is a bug
        ; that looks like the game's fault.
        mov     byte [dsp_reply], 3
        mov     byte [dsp_has_reply], 1
        mov     byte [dsp_reply2], 2
        mov     byte [dsp_has_reply2], 1
        jmp     .done
.not_version:
        cmp     al, 0xE0                ; the identity echo: invert what follows
        je      .expect1
        cmp     al, 0x10                ; a sample straight to the speaker
        je      .expect1
        cmp     al, 0xE4                ; the test register, written
        je      .expect1
        cmp     al, 0x38                ; a byte out of the MIDI port
        je      .expect1
        ; A command whose parameters are not counted is worse than a command
        ; that is not understood: the bytes behind it are then read as
        ; commands themselves, and among the values a length or a period can
        ; take is D3h, which switches the output off.  A game whose sound
        ; stopped for no reason stopped here.
        cmp     al, 0x80                ; a silence of so many samples
        je      .expect2
        cmp     al, 0x74                ; four-bit ADPCM, and its relatives
        jb      .done
        cmp     al, 0x77
        jbe     .expect2
        jmp     .done
.expect2:
        mov     byte [dsp_expect], 2
        mov     byte [dsp_param_n], 0
        jmp     .done
.start_auto:
        mov     byte [sb_auto], 1
        jmp     .started
.start_once:
        mov     byte [sb_auto], 0
        jmp     .started
.stop_auto:
        ; The block being played is finished first on a real chip; here it
        ; stops, which is what the program asked for and a fraction of a
        ; block sooner than it would have got.
        mov     byte [sb_auto], 0
        mov     byte [sb_playing], 0
        jmp     .done
.speaker_status:
        mov     al, [sb_speaker]
        neg     al                      ; FFh on, 00h off
        mov     [dsp_reply], al
        mov     byte [dsp_has_reply], 1
        jmp     .done
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
        ; Where the samples are.  Usually this host watched the game program
        ; the transfer controller and remembered it on the way past - but a
        ; DPMI client at privilege level three has no permission map, its
        ; writes went to the real chip, and nothing was seen.  So if nothing
        ; has ever been seen, ask the chip, and ask it every time: a game
        ; moves its buffer, and a shadow that was right once is worse than no
        ; shadow at all - it plays whatever is at the old address, at full
        ; volume, for as long as the game asks for sound.
        cmp     byte [dma_seen], 0
        jne     .have_dma
        call    dma_read_back
.have_dma:
        ; the page register holds the bits the address cannot
        movzx   ecx, byte [sb_chan]
        movzx   ebx, byte [dma_page + ecx]
        shl     ebx, 16
        movzx   eax, word [dma_addr + ecx*2]
        or      ebx, eax
        mov     [au_src], ebx
        ; How long the buffer is, which is not how long a block is.  A program
        ; that wants to be told half way through - and every one that fills a
        ; buffer while the other half plays does - gives the transfer
        ; controller the whole buffer and the chip half of it, and expects the
        ; second half to follow the first.  Rewinding to the front at the end
        ; of every block would play the first half over and over.
        movzx   eax, word [dma_count + ecx*2]
        inc     eax
        mov     [sb_dma_len], eax
        mov     dword [au_block], 0
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
        mov     byte [dma_seen], 1      ; the writes reach here: no need to ask
        mov     ebx, edx
        cmp     bl, 0x08
        jae     .dma_ctrl
        shr     ebx, 1                          ; the channel it belongs to
        cmp     bl, 2                           ; channel 2 is the floppy and
        je      .not_the_card                   ;  is nobody's music
        mov     [sb_chan], bl
.not_the_card:
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
; dma_read_back: which channel the game is playing on, and where its buffer
;   is.  A client at privilege level three writes the transfer controller
;   itself and this host sees none of it, so the chip is asked afterwards -
;   but which channel to ask is a real question.  BLASTER says one, the
;   game's own configuration says the same one, and DMX programs channel 0
;   regardless of both.  So all three of the eight-bit channels are read -
;   channel 2 is the floppy and is left alone - and the one whose count
;   agrees with the block the DSP was told about is the one being played.
; -----------------------------------------------------------------------------
dma_read_back:
        push    eax
        push    ebx
        push    ecx
        push    edx
        movzx   ecx, word [sb_len]
        inc     ecx                             ; the block the DSP was told
        mov     ebx, 1                          ; what BLASTER says, first
        call    .try
        jnc     .chosen
        xor     ebx, ebx                        ; what DMX does anyway
        call    .try
        jnc     .chosen
        mov     ebx, 3                          ; the other eight-bit one
        call    .try
        jnc     .chosen
        mov     ebx, 1                          ; nothing agreed: the usual
        call    .read
.chosen:
        mov     [sb_chan], bl
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; .try: channel EBX into the shadow, CF=0 if its count agrees with the block
;   the DSP was told to play.  A block of N bytes is a count of N-1, and a
;   buffer is a whole number of blocks; a channel carrying something else -
;   the floppy, or nothing at all - will not divide.
.try:
        call    .read
        or      ecx, ecx
        jz      .no
        movzx   eax, word [dma_count + ebx*2]
        inc     eax
        cmp     eax, ecx
        jb      .no                             ; shorter than one block
        xor     edx, edx
        div     ecx
        or      edx, edx
        jnz     .no
        clc
        ret
.no:    stc
        ret

; .read: channel EBX out of the chip and into the shadow.  Addresses and
;   counts come a byte at a time, low then high, and port 0Ch puts the
;   controller's flip-flop back to the low half before each pair.
.read:
        push    ecx
        mov     edx, ebx
        add     edx, edx                        ; the address port
        xor     al, al
        out     0x0C, al
        in      al, dx
        mov     [dma_addr + ebx*2], al
        in      al, dx
        mov     [dma_addr + ebx*2 + 1], al
        inc     edx                             ; and the count beside it
        xor     al, al
        out     0x0C, al
        in      al, dx
        mov     [dma_count + ebx*2], al
        in      al, dx
        mov     [dma_count + ebx*2 + 1], al
        movzx   edx, byte [dma_page_port + ebx] ; the page is elsewhere, and
        in      al, dx                          ;  the four are not in order
        mov     [dma_page + ebx], al
        pop     ecx
        ret

dma_page_port:  db 0x87, 0x83, 0x81, 0x82

; io_dev_in: DX = port, CL = width -> EAX = what the card would have said
; -----------------------------------------------------------------------------
io_dev_in:
        push    ebx
        inc     dword [io_reads]
        xor     eax, eax
        cmp     dx, 0x08
        jb      io_dma_in               ; where the transfer has got to
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
; How far ahead of the hardware to fill.  Every frame of that is a frame of
; music rendered before the game has said what should be in it, so the lead
; wants to be as short as the pump's clock allows: a couple of its intervals
; and no more.  au_lead is set when the monitor knows what clock it has.
AU_LEAD_SLOW    equ 8192                        ; on the timer alone: 186 ms
AU_LEAD_FAST    equ 1536                        ; on the real-time chip: 35 ms
AU_LEAD_STEP    equ 1024                        ; what falling behind costs

io_pump:
        inc     dword [au_calls]
%ifdef DRLOG
        inc     dword [gs:DRLOG_LIN + 16]
        push    eax
        movzx   eax, word [au_err]
        mov     [gs:DRAUX_LIN + 0], eax
        movzx   eax, byte [au_have]
        mov     [gs:DRAUX_LIN + 4], eax
        mov     eax, [opl_writes]
        mov     [gs:DRAUX_LIN + 8], eax
%ifdef HAVE_OPL
        movzx   eax, byte [OPL_LIVE]
%else
        xor     eax, eax
%endif
        mov     [gs:DRAUX_LIN + 12], eax
        movzx   eax, byte [sb_playing]
        mov     [gs:DRAUX_LIN + 16], eax
        mov     eax, [sb_blocks]
        mov     [gs:DRAUX_LIN + 20], eax
        movzx   eax, byte [sb_speaker]
        mov     [gs:DRAUX_LIN + 24], eax
        mov     eax, [au_ring]                  ; and where it is all going
        mov     [gs:DRAUX_LIN + 32], eax
        mov     eax, [au_bytes]
        mov     [gs:DRAUX_LIN + 36], eax
        mov     eax, [au_lpib]
        mov     eax, [gs:eax]
        mov     [gs:DRAUX_LIN + 40], eax
        mov     eax, [au_wpos]
        mov     [gs:DRAUX_LIN + 44], eax
        pop     eax
%endif
        cmp     byte [au_have], 0
        je      .out
        inc     dword [au_ready]
%ifdef DRLOG
        inc     dword [gs:DRLOG_LIN + 20]
%endif
        pushad
        OPL_ENTER                               ; the synthesiser's stack
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
%ifdef DRLOG
        inc     dword [gs:DRLOG_LIN + 24]
%endif
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
        cmp     eax, [au_lead]
        jb      .short                          ; not enough: fill it up
        ; In a ring, so many frames ahead and the same number behind are the
        ; one number.  More in hand than was ever asked for is therefore not
        ; a lead at all - it is the hardware having overtaken us and the
        ; difference having wrapped.  Read that as a lead and the pump would
        ; decide it had plenty, fill nothing, and never catch up again: the
        ; ring would play the same stale second round and round, which is
        ; what a tune rendered into it sounds like when it goes wrong.
        mov     edx, [au_lead]
        add     edx, edx
        cmp     eax, edx
        jb      .done                           ; genuinely ahead
        mov     eax, ebx                        ; behind: start again, just
        add     eax, 256                        ;  in front of the hardware
        xor     edx, edx
        div     ecx
        mov     [au_wpos], edx
        inc     dword [au_late]
        ; Falling behind is a click, and a lead too short to be kept is a
        ; click every time round.  So the lead is not a constant: it starts
        ; at what the clock should allow and grows whenever the machine
        ; turns out not to manage it.  A fast machine keeps a short lead and
        ; music made close to the moment it is heard; a slow one settles at
        ; whatever it can hold without breaking up.
        mov     eax, [au_lead]
        add     eax, AU_LEAD_STEP
        cmp     eax, AU_LEAD_SLOW
        jbe     .lead_ok
        mov     eax, AU_LEAD_SLOW
.lead_ok:
        mov     [au_lead], eax
        xor     edx, edx                        ; and nothing in hand
.short:
        mov     eax, [au_lead]
        sub     eax, edx
        mov     edi, eax                        ; frames to lay down
        add     [au_frames], edi
%ifdef DRLOG
        add     [gs:DRLOG_LIN + 28], edi
%endif

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
        ; on to the next block, and round to the front at the end of the buffer
        mov     dword [au_spos], 0
        push    ecx
        mov     eax, [au_block]
        add     eax, ecx                        ; the block that just finished
        cmp     eax, [sb_dma_len]
        jb      .block_ok
        xor     eax, eax
.block_ok:
        mov     [au_block], eax
        pop     ecx
        inc     dword [sb_blocks]
        mov     byte [sb_irq_owed], 1
        xor     eax, eax
.have_sample:
        add     eax, [au_block]
        add     eax, [au_src]
        movzx   eax, byte [gs:eax]
        cmp     al, 0x80
        je      .byte_quiet
        or      al, al
        jz      .byte_zero
        inc     dword [sb_sound_b]
        jmp     .byte_counted
.byte_zero:
        inc     dword [sb_zero_b]
        jmp     .byte_counted
.byte_quiet:
        inc     dword [sb_quiet_b]
.byte_counted:
        sub     eax, 128                        ; the card's silence is 80h
        shl     eax, 8                          ; and this stream's is zero
        push    eax                             ; the loudest of them, for the
        or      eax, eax                        ;  report: a buffer of silence
        jge     .sample_up                      ;  and a speaker switched off
        neg     eax                             ;  look the same from outside
.sample_up:
        cmp     eax, [sb_peak]
        jbe     .sample_same
        mov     [sb_peak], eax
.sample_same:
        pop     eax
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
        push    eax
        mov     eax, ebx
        or      eax, eax
        jge     .frame_up
        neg     eax
.frame_up:
        cmp     eax, [au_peak]
        jbe     .frame_same
        mov     [au_peak], eax
.frame_same:
        pop     eax
%ifdef DRLOG
        push    eax                             ; the loudest frame so far
        mov     eax, ebx
        or      eax, eax
        jge     .positive
        neg     eax
.positive:
        cmp     eax, [gs:DRAUX_LIN + 28]
        jbe     .not_a_peak
        mov     [gs:DRAUX_LIN + 28], eax
.not_a_peak:
        pop     eax
%endif
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
        call    dma_advance                     ; and it is asked where it is
        OPL_LEAVE
        popad
.out:   ret

; -----------------------------------------------------------------------------
; The transfer controller, asked where it has got to.  Only the monitor sees
; these: a client at privilege level three reads the real chip, which is why
; dma_advance keeps that one honest as well.
; -----------------------------------------------------------------------------
io_dma_in:
        push    ecx
        mov     ebx, edx
        shr     ebx, 1                          ; the channel it belongs to
        cmp     bl, [sb_chan]
        jne     .shadow
        call    dma_position
        or      ecx, ecx
        jz      .shadow
        test    dl, 1
        jnz     .what_is_left
        mov     eax, [au_src]                   ; where it would have reached
        add     eax, ecx
        jmp     .half
.what_is_left:
        mov     eax, [sb_dma_len]
        sub     eax, ecx
        dec     eax
        jmp     .half
.shadow:
        test    dl, 1
        jnz     .shadow_count
        movzx   eax, word [dma_addr + ebx*2]
        jmp     .half
.shadow_count:
        movzx   eax, word [dma_count + ebx*2]
.half:
        cmp     byte [dma_ff], 0                ; low half first, as it writes
        jne     .high
        mov     byte [dma_ff], 1
        and     eax, 0xFF
        jmp     .out
.high:
        mov     byte [dma_ff], 0
        shr     eax, 8
        and     eax, 0xFF
.out:   pop     ecx
        pop     ebx
        ret

; -----------------------------------------------------------------------------
; dma_position: how far into the buffer the card has played, in bytes.
;   Returns it in ECX; zero if nothing is playing.
; -----------------------------------------------------------------------------
dma_position:
        push    eax
        push    edx
        xor     ecx, ecx
        cmp     byte [sb_playing], 0
        je      .done
        cmp     dword [sb_dma_len], 0
        je      .done
        mov     eax, [au_spos]
        shr     eax, 16
        add     eax, [au_block]
        xor     edx, edx
        div     dword [sb_dma_len]
        mov     ecx, edx
.done:  pop     edx
        pop     eax
        ret

; -----------------------------------------------------------------------------
; dma_advance: move the transfer controller's address on to where the card has
;   got to.
;
;   A real card is fed by that chip, so the chip's own address register says
;   how far through the buffer the sound has been played, and a program that
;   wants to know where to write next has only to read it.  DMX does exactly
;   that - the trace shows it reading the address back a few instructions
;   after it programmed it.  On this card nothing feeds anything: the chip was
;   programmed by the game, the game's writes went straight to it, and then it
;   sat there, because there is no card to raise a request and nothing ever
;   transfers.  So the program asks where the sound has got to, is told
;   "nowhere", concludes that nothing has been consumed and that there is no
;   room to mix into, and writes silence for as long as the game runs.
;
;   Nothing here is transferring, so the register is ours to keep honest: it
;   is written each time the pump runs with the position the card would have
;   reached.  Writing it sets the base as well as the current address, which
;   would matter if the chip ever reloaded - it cannot, because it never
;   counts down - and the base the game gave is remembered in au_src anyway.
; -----------------------------------------------------------------------------
dma_advance:
        cmp     byte [sb_playing], 0
        je      .none
        cmp     dword [sb_dma_len], 0
        je      .none
        push    eax
        push    ebx
        push    ecx
        push    edx
        movzx   ebx, byte [sb_chan]
        mov     eax, [au_spos]
        shr     eax, 16
        add     eax, [au_block]
        xor     edx, edx
        div     dword [sb_dma_len]
        mov     ecx, edx                        ; how far in, in bytes
        mov     eax, ebx
        or      al, 0x04
        out     0x0A, al                        ; held, while it is written
        xor     al, al
        out     0x0C, al                        ; and the flip-flop to the low
        mov     edx, ebx                        ;  half of what follows
        add     edx, edx
        mov     eax, [au_src]
        add     eax, ecx
        out     dx, al
        mov     al, ah
        out     dx, al
        inc     edx                             ; what is left of the buffer
        mov     eax, [sb_dma_len]
        sub     eax, ecx
        dec     eax
        out     dx, al
        mov     al, ah
        out     dx, al
        mov     eax, ebx
        out     0x0A, al                        ; and let it go again
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
.none:  ret

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
        call    io_card_reset           ; a card that has just been switched
        mov     byte [au_have], 0       ;  on, whatever the last one left
        mov     dx, au_info
        mov     ah, 0xF0
        int     0x21
        mov     [au_err], ax
        jc      .none
        mov     byte [au_have], 1
        mov     word [au_err], 0xFFFF
        call    io_audio_hush           ; nothing of the last game in it
.none:  pop     dx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; io_audio_hush: silence in the ring, now.
;   The machine's audio hardware plays round and round whatever is in the ring
;   and never stops asking for more.  Between a game's last sample and the
;   stream being handed back nobody is writing it - the report below takes a
;   moment, and a moment is three times round - so what is heard is the last
;   fraction of a second of the game, repeating.  That is the glitch at the
;   end of a game, and the same thing at the start is the end of the last one.
;
;   So the ring is filled with silence at both ends.  The kernel does this too
;   when the stream stops, but that is later than the report and later than
;   the ear; and an older kernel does not do it at all.
;
;   The ring is always in the first megabyte - the transfer hardware that
;   reads it cannot see past that - so real mode can reach it, and a paragraph
;   boundary is where it always starts.
; -----------------------------------------------------------------------------
io_audio_hush:
        cmp     byte [au_have], 0
        je      .none
        push    eax
        push    cx
        push    di
        push    es
        mov     eax, [au_ring]
        or      eax, eax
        jz      .done
        cmp     eax, 0x00100000
        jae     .done
        push    eax
        mov     eax, [au_bytes]                 ; 64 KB, exactly: its last
        shr     eax, 1                          ;  word is the segment's last
        jz      .too_odd
        cmp     eax, 0x10000                    ; and a word count is sixteen
        ja      .too_odd                        ;  bits, no more
        mov     cx, ax
        pop     eax
        shr     eax, 4
        mov     es, ax
        xor     di, di
        xor     ax, ax
        rep     stosw
        wbinvd                                  ; the controller reads memory
        jmp     .done
.too_odd:
        pop     eax
.done:  pop     es
        pop     di
        pop     cx
        pop     eax
.none:  ret

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

; io_buffer_head: the first four bytes of the game's buffer, as the card would
;   have read them.  80808080h is a buffer full of silence, which is what a
;   game writes when nothing is making a noise; 00000000h is a buffer nothing
;   ever wrote, which means the address came back wrong.  Told apart, they are
;   two different faults with one symptom.
io_buffer_head:
        push    bx
        push    es
        movzx   bx, byte [sb_chan]
        push    bx
        movzx   eax, byte [dma_page + bx]
        shl     eax, 16
        pop     bx
        shl     bx, 1
        movzx   ebx, word [dma_addr + bx]
        or      eax, ebx
        cmp     eax, 0x000FFFF0                 ; real mode cannot look higher
        jae     .too_high
        mov     bx, ax
        and     bx, 0x000F
        shr     eax, 4
        mov     es, ax
        mov     eax, [es:bx]
        jmp     .done
.too_high:
        or      eax, -1
.done:  pop     es
        pop     bx
        ret

io_report:
        call    io_audio_hush           ; before the report, not after it
        cmp     dword [io_writes], 0
        jne     .say
        cmp     dword [io_reads], 0
        je      .none
; Text mode first, if the program left the screen somewhere else.  The shell
; puts the screen back a moment after this and takes anything written to a
; graphics screen with it, so a report written there is a report nobody
; reads - and after a game, a graphics screen is the usual case.
.say:   push    ax
        mov     ah, 0x0F
        int     0x10
        cmp     al, 3
        je      .in_text
        mov     ax, 0x0003
        int     0x10
.in_text:
        pop     ax
        mov     si, msg_io_w
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
        mov     si, msg_io_late
        mov     eax, [au_late]
        call    io_say
        mov     si, msg_io_blk
        mov     eax, [sb_blocks]
        call    io_say
        mov     si, msg_io_go
        mov     eax, [sb_starts]
        call    io_say
        ; where the samples are: the page register carries the top bits the
        ; address register has no room for
        movzx   ecx, byte [sb_chan]
        movzx   eax, byte [dma_page + ecx]
        shl     eax, 16
        movzx   ebx, word [dma_addr + ecx*2]
        or      eax, ebx
        mov     si, msg_io_buf
        call    io_say
        movzx   ecx, byte [sb_chan]
        movzx   eax, word [dma_count + ecx*2]
        mov     si, msg_io_cnt
        call    io_say
        movzx   eax, byte [sb_chan]
        mov     si, msg_io_ch
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
        movzx   eax, byte [sb_speaker]
        mov     si, msg_io_spk
        call    io_say
        mov     eax, [sb_peak]
        mov     si, msg_io_pk
        call    io_say
        mov     eax, [au_peak]
        mov     si, msg_io_apk
        call    io_say
        mov     eax, [sb_sound_b]
        mov     si, msg_io_bs
        call    io_say
        mov     eax, [sb_quiet_b]
        mov     si, msg_io_bq
        call    io_say
        mov     eax, [sb_zero_b]
        mov     si, msg_io_bz
        call    io_say
        mov     eax, [sb_dma_len]
        mov     si, msg_io_blen
        call    io_say
        call    io_buffer_head          ; the first four bytes of it
        mov     si, msg_io_head
        call    io_say
.none:
        cmp     dword [dr_traps], 0
        je      .no_watch
        mov     si, msg_io_dr
        mov     eax, [dr_traps]
        call    io_say
        mov     si, msg_io_drc
        mov     eax, [dr_ours]
        call    io_say
        cmp     dword [dr_odd], 0
        je      .no_odd
        mov     si, msg_io_dro
        mov     eax, [dr_odd]
        call    io_say
.no_odd:
        mov     si, msg_io_irq
        mov     eax, [dr_card_irqs]
        call    io_say
        cmp     dword [card_irq_late], 0
        je      .no_watch
        mov     si, msg_io_irql
        mov     eax, [card_irq_late]
        call    io_say
.no_watch:
        ret

msg_io_calls:   db "SB: the pump was asked       ", 0
msg_io_ready:   db "SB: ...with a stream to fill ", 0
msg_io_frames:  db "SB: frames it laid down      ", 0
msg_io_late:    db "SB: times it fell behind     ", 0
msg_io_blk:     db "SB: blocks played out        ", 0
msg_io_ring:    db "SB: the audio ring at        ", 0
msg_io_rate:    db "SB: the stream plays at      ", 0
msg_io_w:       db "SB: writes to the card       ", 0
msg_io_r:       db "SB: reads from it            ", 0
msg_io_fm:      db "SB: writes to the synth      ", 0
msg_io_go:      db "SB: transfers begun          ", 0
msg_io_buf:     db "SB: the buffer at            ", 0
msg_io_cnt:     db "SB: the controller's count   ", 0
msg_io_ch:      db "SB: on transfer channel      ", 0
msg_io_len:     db "SB: the block the DSP wants  ", 0
msg_io_tc:      db "SB: the time constant        ", 0
msg_io_auto:    db "SB: keeps playing by itself  ", 0
msg_io_spk:     db "SB: the speaker is on        ", 0
msg_io_pk:      db "SB: the loudest sample in it ", 0
msg_io_apk:     db "SB: the loudest frame out    ", 0
msg_io_head:    db "SB: the buffer begins        ", 0
msg_io_bs:      db "SB: bytes that were a sound  ", 0
msg_io_bq:      db "SB: bytes of quiet           ", 0
msg_io_bz:      db "SB: bytes nobody ever wrote  ", 0
msg_io_blen:    db "SB: the buffer is this long  ", 0
msg_io_dr:      db "SB: debug traps              ", 0
msg_io_drc:     db "SB: ...on the card's ports   ", 0
msg_io_dro:     db "SB: ...that made no sense    ", 0
msg_io_irq:     db "SB: the card's interrupts    ", 0
msg_io_irql:    db "SB: ...owed, nobody listened ", 0

; ---- what the card remembers ------------------------------------------------
; Everything from io_state to io_state_end is cleared between programs, so
; each run of a game meets a card that has just been switched on.
;
; The eight bytes in front of it are there to be searched for.  A program
; that hangs never reaches io_report, and the counters are the only thing
; that says what it was doing; tools/sbpeek.py finds this in a memory dump
; and reads them out.  Eight bytes is a small price for being able to ask a
; hung machine what happened.
; The lead a card starts a program with.  Not part of what is cleared: it
; says what clock the pump has, which is a property of the monitor and not
; of the program that just ran.
au_lead_start:  dd AU_LEAD_SLOW
io_probe:       db "SBCARD!!"
io_state:
io_writes:      dd 0                    ; counted so a test can see them arrive
io_reads:       dd 0
; The debug registers, which is how a client at privilege level three has its
; ports watched.  On a machine that cannot be stopped and dumped, these three
; numbers are the difference between "the watch never fired" and "it fired and
; the card said the wrong thing", and there is no other way to tell.
; What the buffer held, byte by byte, as the card walked through it.  A game
; that is not making a noise writes 80h everywhere; memory nobody has written
; is 00h; anything else is a sound.  Three numbers, and between them there is
; no question left about whether the samples ever arrived.
sb_quiet_b:     dd 0                    ; bytes of 80h: silence, deliberately
sb_zero_b:      dd 0                    ; bytes of 00h: nothing was ever there
sb_sound_b:     dd 0                    ; and bytes that were a sound
sb_peak:        dd 0                    ; the loudest sample the card held
au_peak:        dd 0                    ; ...and the loudest frame laid down
dr_traps:       dd 0                    ; debug exceptions, all told
dr_ours:        dd 0                    ; ...that were one of the card's ports
dr_odd:         dd 0                    ; ...that could not be read back
dr_card_irqs:   dd 0                    ; the card's interrupt, given out
card_irq_late:  dd 0                    ; ...owed, and nobody was listening
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
sb_chan:        db 1                    ; the channel the game is playing on
dma_seen:       db 0                    ; the transfer controller was watched
dma_ff:         db 0                    ; which half of an address comes next
dma_mask:       db 0
                align 2
dma_addr:       times 4 dw 0
dma_count:      times 4 dw 0
dma_page:       times 4 db 0
dma_mode:       times 4 db 0
; what the kernel hands back: the ring, its size, where the hardware says it
; has got to, and the rate it plays at
%ifdef OPL_OWN_STACK
                align 4
opl_esp:        dd 0                    ; the host's own stack, while the
opl_ss:         dw 0                    ;  synthesiser has one of its own
                align 16
opl_stack:      times 2048 db 0
opl_stack_top:
%endif
au_have:        db 0
au_err:         dw 0xEEEE
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
au_block:       dd 0                    ; where in it the block being played is
sb_dma_len:     dd 0                    ; and how long the whole of it is
sb_blocks:      dd 0                    ; blocks played out
au_calls:       dd 0                    ; times the pump was asked to run
au_ready:       dd 0                    ; times it had a stream to fill
au_frames:      dd 0                    ; frames it laid down
au_primed:      db 0                    ; the write position has been placed
                align 4
au_lead:        dd AU_LEAD_SLOW         ; how far ahead of the hardware to fill
au_late:        dd 0                    ; times the hardware overtook the pump
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
