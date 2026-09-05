; =============================================================================
;  spkbrdg.asm - the PC speaker, heard through the HD Audio codec
; -----------------------------------------------------------------------------
;  Old DOS programs make sound by programming timer channel 2 and opening the
;  gate in port 61h.  On a modern laptop that signal goes nowhere: there is no
;  beeper, and the chipset does not feed the line into the audio codec.
;
;  This bridge has no clock of its own and never touches the timer.  It arms
;  the processor's hardware I/O breakpoints on ports 42h, 43h and 61h, so
;  every write a program makes to them arrives as a debug trap once the
;  instruction has done its work.  The trap keeps a shadow of what the timer
;  was told, and whenever that shadow changes the note, it refills the whole
;  HD Audio ring buffer with the new square wave.  The controller then loops
;  that buffer until the next change, so a held note plays on with no further
;  help from anyone.
;
;  Nothing here runs periodically.  Two earlier designs used a clock
;  interrupt and both starved the machine's own interrupts on a laptop whose
;  legacy ports are emulated in firmware: the first killed the keyboard, the
;  second stopped the timer tick.  This one only runs when the program being
;  listened to writes to a port, which is a few times a second.
;
;  The buffer holds a whole number of cycles, so the loop is seamless; that
;  quantises the pitch to about 2.7 Hz, a fraction of a semitone.
; =============================================================================

SPK_RING_FRAMES equ 16384                       ; the ring is exactly 64 KB
SPK_AMPL        equ 6000
SPK_RATE        equ 44100
SPK_PIT_HZ      equ 1193182
SPK_CYC_NUM     equ SPK_PIT_HZ * SPK_RING_FRAMES / SPK_RATE  ; cycles = this / N
SPK_DR7         equ 0x02220015                  ; DR0-2 on, I/O, 1 byte
SPK_DR7_NO61    equ 0x00220005                  ; ...without the one on 61h
SPK_DR7_NO43    equ 0x02020011                  ; ...without the one on 43h
SPK_DR7_42      equ 0x00020001                  ; ...only the one on 42h
SPK_GUARD_N     equ 4096                        ; traps between cost reviews
SPK_GUARD_SHIFT equ 2                           ; stand down past a quarter
SPK_MIN_GAP     equ 4000000                     ; cycles between tone refills

; -----------------------------------------------------------------------------
; spk_bridge_start: begin listening.  CF=1 on failure with AX = 1 (no HD
;   Audio) or 2 (the processor does not trap port I/O).
; -----------------------------------------------------------------------------
spk_bridge_start:
        pushad
        push    es
        cmp     byte [spk_on], 0
        jne     .already
        push    cs
        pop     es
        mov     di, spk_info
        call    snd_stream_start                ; 44.1 kHz stereo, looping
        jc      .no_audio
        mov     eax, 1
        cpuid
        mov     eax, edx
        shr     eax, 19
        and     al, 1
        mov     [spk_clflush], al               ; CLFLUSH for the DMA buffer
        mov     eax, edx
        shr     eax, 4
        and     al, 1
        mov     [spk_tsc_ok], al
        ; ---- the shadow of the timer: nothing known yet ----
        mov     byte [spk_mode], 0
        mov     byte [spk_access], 0
        mov     byte [spk_toggle], 0
        mov     byte [spk_gate], 0
        mov     word [spk_reload], 0
        mov     dword [spk_traps], 0
        mov     dword [spk_refills], 0
        mov     dword [spk_trap_n], 0
        mov     dword [spk_w42], 0
        mov     dword [spk_w43], 0
        mov     dword [spk_w61], 0
        mov     word [spk_n42], 0
        mov     word [spk_n43], 0
        mov     word [spk_n61], 0
        mov     byte [spk_dropped61], 0
        mov     byte [spk_dropped43], 0
        mov     dword [spk_load], 0
        mov     dword [spk_worst], 0
        mov     dword [spk_per_trap], 0
        mov     byte [spk_panicked], 0
        call    spk_refill                      ; start from silence
        ; ---- the debug trap, and the breakpoints ----
        cli
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     eax, [1*4]
        mov     [cs:spk_old_01], eax
        mov     word [1*4], spk_db
        mov     [1*4+2], cs
        pop     ds
        mov     eax, cr4
        or      eax, 0x08                       ; CR4.DE: I/O breakpoints
        mov     cr4, eax
        xor     eax, eax
        mov     dr6, eax
        mov     eax, 0x42
        mov     dr0, eax
        mov     eax, 0x43
        mov     dr1, eax
        mov     eax, 0x61
        mov     dr2, eax
        mov     eax, SPK_DR7
        mov     dr7, eax
        cmp     byte [spk_tsc_ok], 0
        je      .no_tsc
        rdtsc
        mov     [spk_last_tsc], eax
        mov     [spk_storm_tsc], eax
        mov     [spk_t0], eax
.no_tsc:
        ; ---- self-test: this read of the gate must be trapped ----
        in      al, 0x61
        cmp     dword [spk_traps], 0
        je      .no_traps
        and     al, 0x03
        mov     [spk_gate], al                  ; and it tells us where we start
        mov     byte [spk_on], 1
        sti
.already:
        pop     es
        popad
        clc
        ret
.no_traps:
        call    spk_disarm
        sti
        call    snd_stream_stop
        pop     es
        popad
        mov     ax, 2
        stc
        ret
.no_audio:
        pop     es
        popad
        mov     ax, 1
        stc
        ret

; spk_disarm: breakpoints off, the debug vector as it was
spk_disarm:
        push    eax
        push    ds
        xor     eax, eax
        mov     dr7, eax
        mov     dr6, eax
        xor     ax, ax
        mov     ds, ax
        mov     eax, [cs:spk_old_01]
        or      eax, eax
        jz      .done
        mov     [1*4], eax
.done:  pop     ds
        pop     eax
        ret

; -----------------------------------------------------------------------------
; spk_bridge_stop: stop listening and give the audio stream back.
; -----------------------------------------------------------------------------
spk_bridge_stop:
        pushad
        cmp     byte [spk_on], 0
        je      .done
        cli
        mov     byte [spk_on], 0
        call    spk_disarm
        sti
        call    snd_stream_stop
.done:  popad
        ret

; spk_bridge_resume: start again if the user asked for the bridge
spk_bridge_resume:
        cmp     byte [spk_want], 0
        je      .done
        cmp     byte [spk_on], 0
        jne     .done
        call    spk_bridge_start
.done:  ret

; -----------------------------------------------------------------------------
; spk_refill: rebuild the ring buffer from the shadow.  A whole number of
;   cycles is written across it, so the controller's loop is seamless.
; -----------------------------------------------------------------------------
spk_refill:
        pushad
        push    es
        inc     dword [spk_refills]
        mov     es, [pcm_seg]
        xor     di, di
        cmp     byte [spk_gate], 0x03           ; is a note sounding?
        jne     .silence
        mov     al, [spk_mode]
        and     al, 0x03                        ; modes 2/3 (and 6/7): a tone
        cmp     al, 2
        jb      .silence
        movzx   ebx, word [spk_reload]
        cmp     ebx, 2
        jb      .silence
        mov     eax, SPK_CYC_NUM                ; cycles that fit the ring
        xor     edx, edx
        div     ebx
        or      eax, eax
        jnz     .have_cycles
        inc     eax
.have_cycles:
        cmp     eax, SPK_RING_FRAMES / 4        ; past hearing: call it quiet
        ja      .silence
        shl     eax, 2                          ; phase step per frame, 16.16
        mov     edx, eax
        xor     ebx, ebx                        ; phase
        mov     cx, SPK_RING_FRAMES
.frame: mov     ax, -SPK_AMPL
        test    bh, 0x80                        ; bit 15 of the phase: square
        jz      .store
        mov     ax, SPK_AMPL
.store: mov     [es:di], ax
        mov     [es:di+2], ax
        add     ebx, edx
        add     di, 4                           ; wraps with the 64 KB ring
        loop    .frame
        jmp     .flush
.silence:
        xor     eax, eax
        mov     cx, SPK_RING_FRAMES
        rep     stosd
.flush:
        ; ---- make sure the controller sees it ----
        cmp     byte [spk_clflush], 0
        je      .writeback
        xor     di, di
        mov     cx, SPK_RING_FRAMES / 16        ; one per 64-byte line
.line:  clflush [es:di]
        add     di, 64
        loop    .line
        jmp     .done
.writeback:
        wbinvd
.done:  pop     es
        popad
        ret

; -----------------------------------------------------------------------------
; spk_db: the debug trap.  Runs after a program has done an IN or OUT on one
;   of the watched ports.  It moves to its own stack before pushing anything,
;   because the code it interrupted may be a BIOS routine with very little.
; -----------------------------------------------------------------------------
spk_db:
        mov     [cs:spk_db_ax], ax
        mov     [cs:spk_db_ss], ss
        mov     [cs:spk_db_sp], sp
        mov     ax, cs
        mov     ss, ax
        mov     sp, spk_db_stack_top
        push    ds                              ; from here the stack is ours
        push    es
        push    bp
        push    ebx
        push    ecx
        push    dx
        push    si
        mov     ds, ax
        cmp     byte [spk_tsc_ok], 0
        je      .no_t0
        rdtsc
        mov     [spk_t0], eax
.no_t0: mov     es, [spk_db_ss]                 ; reach their frame through ES
        mov     bp, [spk_db_sp]                 ; [es:bp] = IP, [es:bp+2] = CS
        mov     eax, dr6
        mov     dl, al                          ; DL = which breakpoint
        xor     ebx, ebx
        mov     dr6, ebx                        ; ready for the next one
        test    dl, 0x07
        jz      .done                           ; some other debug event
        inc     dword [spk_traps]
        test    dl, 0x04                        ; tally this window by port
        jz      .not61
        inc     word [spk_n61]
        jmp     .tallied
.not61: test    dl, 0x02
        jz      .not43
        inc     word [spk_n43]
        jmp     .tallied
.not43: inc     word [spk_n42]
.tallied:
        cmp     byte [spk_on], 0
        je      .done
        ; ---- was it a write?  look at the instruction that just ran ----
        mov     si, [es:bp+2]                   ; their CS
        mov     ax, si
        mov     si, [es:bp]                     ; their IP
        mov     es, ax
        mov     al, [es:si-1]
        cmp     al, 0xEE                        ; out dx, al
        je      .write
        cmp     al, 0xEF                        ; out dx, ax
        je      .write
        mov     al, [es:si-2]
        cmp     al, 0xE6                        ; out imm8, al
        je      .write
        cmp     al, 0xE7                        ; out imm8, ax
        jne     .done                           ; a read: nothing changed
.write: mov     al, [spk_db_ax]                 ; the byte that went out
        test    dl, 0x02
        jnz     .control_w                      ; port 43h
        test    dl, 0x01
        jnz     .count_w                        ; port 42h
        ; ---- port 61h: the gate ----
        inc     dword [spk_w61]
        and     al, 0x03
        cmp     al, [spk_gate]
        je      .done                           ; nothing actually changed
        mov     [spk_gate], al                  ; opening or closing the gate
        jmp     .refill_now                     ;  always takes effect at once
.control_w:
        inc     dword [spk_w43]
        call    spk_regate                      ; in case 61h is not watched
        mov     al, [spk_db_ax]
.control:
        mov     ah, al
        and     ah, 0xC0
        cmp     ah, 0x80                        ; channel 2?
        jne     .done                           ; another channel, or read-back
        mov     ah, al
        shr     ah, 4
        and     ah, 0x03
        jz      .done                           ; a latch: leaves the count alone
        mov     [spk_access], ah
        mov     ah, al
        shr     ah, 1
        and     ah, 0x07
        mov     [spk_mode], ah
        mov     byte [spk_toggle], 0
        jmp     .done
.count_w:
        inc     dword [spk_w42]
        call    spk_regate
        mov     al, [spk_db_ax]
.count: mov     ah, [spk_access]
        cmp     ah, 1
        je      .low_only
        cmp     ah, 2
        je      .high_only
        cmp     byte [spk_toggle], 0
        jne     .high
        mov     [spk_lo], al                    ; first byte: the low one
        mov     byte [spk_toggle], 1
        jmp     .done                           ; wait for the other half
.high:  mov     ah, al
        mov     al, [spk_lo]
        mov     [spk_reload], ax
        mov     byte [spk_toggle], 0
        jmp     .maybe_refill
.low_only:
        xor     ah, ah
        mov     [spk_reload], ax
        jmp     .maybe_refill
.high_only:
        mov     ah, al
        xor     al, al
        mov     [spk_reload], ax
        ; fall through

; a new note: rebuild the buffer, but not so often that a program sweeping
; its pitch could spend all its time in here
.maybe_refill:
        cmp     byte [spk_gate], 0x03           ; silent anyway: the new note
        jne     .done                           ;  will be built when it starts
        cmp     byte [spk_tsc_ok], 0
        je      .refill_now
        rdtsc
        mov     ebx, eax
        sub     eax, [spk_last_tsc]
        cmp     eax, SPK_MIN_GAP                ; a program sweeping its pitch
        jb      .done                           ;  must not live in here
        mov     [spk_last_tsc], ebx
.refill_now:
        cmp     byte [spk_tsc_ok], 0
        je      .fill
        rdtsc
        mov     [spk_last_tsc], eax
.fill:  call    spk_refill
.done:  call    spk_cost_check
        pop     si
        pop     dx
        pop     ecx
        pop     ebx
        pop     bp
        pop     es
        pop     ds
        mov     ax, [cs:spk_db_ax]              ; exactly as we found it
        mov     ss, [cs:spk_db_ss]              ; back to their stack, which
        mov     sp, [cs:spk_db_sp]              ;  holds only the return frame
        iret

; spk_regate: refresh the gate by reading the port, for when it is no longer
;   watched.  Reading it has no side effect of any kind.
spk_regate:
        cmp     byte [spk_dropped61], 0
        je      .done
        push    ax
        in      al, 0x61
        and     al, 0x03
        mov     [spk_gate], al
        pop     ax
.done:  ret

; spk_record_cost: EAX = the window's length in cycles.  Remembers the worst
;   share of the machine the traps have taken, and what one costs on average.
spk_record_cost:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ecx, [spk_load]
        shr     ecx, 12                         ; per trap, over 4096 of them
        mov     [spk_per_trap], ecx
        xor     edx, edx
        mov     ecx, 100
        div     ecx                             ; a hundredth of the window
        mov     ecx, eax
        or      ecx, ecx
        jz      .done
        mov     eax, [spk_load]
        xor     edx, edx
        div     ecx                             ; ...so this is a percentage
        cmp     eax, [spk_worst]
        jbe     .done
        mov     [spk_worst], eax
.done:  pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; -----------------------------------------------------------------------------
; spk_cost_check: what fraction of the machine are these traps costing?
;   Programs of this vintage poll the timer thousands of times a second and
;   that is fine - a trap is about a microsecond.  Only if the traps add up
;   to a real share of the processor is anything done, and even then the
;   first move is to stop watching whichever port is being polled, not to
;   give up: a game that reads the timer for its own clock still writes it
;   for its notes, and the notes are all this needs.
; -----------------------------------------------------------------------------
spk_cost_check:
        cmp     byte [spk_tsc_ok], 0
        je      .done
        rdtsc
        sub     eax, [spk_t0]
        add     [spk_load], eax                 ; time spent in here
        inc     dword [spk_trap_n]
        cmp     dword [spk_trap_n], SPK_GUARD_N
        jb      .done
        mov     dword [spk_trap_n], 0
        rdtsc
        mov     ebx, eax
        sub     eax, [spk_storm_tsc]            ; time the window took
        mov     [spk_storm_tsc], ebx
        push    eax
        call    spk_record_cost                 ; keep the worst for the record
        pop     eax
        mov     ebx, eax                        ; three quarters of it
        shr     eax, 1
        shr     ebx, 2
        add     eax, ebx
        cmp     [spk_load], eax
        mov     dword [spk_load], 0
        jbe     .fresh                          ; well within its keep
        ; Too expensive.  Give up a port, in the order they can be spared:
        ; the control port first (the note itself is written to 42h, so that
        ; one can never go), then the gate, and only then stand down.
        cmp     byte [spk_dropped43], 0
        jne     .try61
        mov     byte [spk_dropped43], 1         ; the control port: from here
        cmp     byte [spk_mode], 0              ;  on assume the usual square
        jne     .have_mode                      ;  wave and two-byte divisor
        mov     byte [spk_mode], 3
.have_mode:
        cmp     byte [spk_access], 0
        jne     .set_dr7
        mov     byte [spk_access], 3
.set_dr7:
        mov     eax, SPK_DR7_NO43
        cmp     byte [spk_dropped61], 0
        je      .arm
        mov     eax, SPK_DR7_42
.arm:   mov     dr7, eax
        jmp     .fresh
.try61: cmp     byte [spk_dropped61], 0
        jne     .stand_down
        mov     byte [spk_dropped61], 1
        mov     eax, SPK_DR7_NO61
        cmp     byte [spk_dropped43], 0
        je      .arm61
        mov     eax, SPK_DR7_42
.arm61: mov     dr7, eax
        jmp     .fresh
.stand_down:
        call    spk_disarm
        mov     byte [spk_on], 0
        mov     byte [spk_want], 0
        mov     byte [spk_panicked], 1
.fresh: mov     word [spk_n42], 0               ; a fresh window
        mov     word [spk_n43], 0
        mov     word [spk_n61], 0
.done:  ret

section .data
spk_on:         db 0
spk_want:       db 0
spk_clflush:    db 0
spk_tsc_ok:     db 0
spk_panicked:   db 0
spk_dropped61:  db 0
spk_dropped43:  db 0
; the shadow of timer channel 2 and the speaker gate
spk_gate:       db 0
spk_mode:       db 0
spk_access:     db 0
spk_toggle:     db 0
spk_lo:         db 0
                align 2
spk_reload:     dw 0
spk_db_ax:      dw 0
spk_db_ss:      dw 0
spk_db_sp:      dw 0
                align 4
spk_old_01:     dd 0
spk_traps:      dd 0
spk_refills:    dd 0
spk_trap_n:     dd 0
spk_last_tsc:   dd 0
spk_storm_tsc:  dd 0
spk_t0:         dd 0
spk_load:       dd 0
spk_worst:      dd 0
spk_per_trap:   dd 0
spk_w42:        dd 0
spk_w43:        dd 0
spk_w61:        dd 0
spk_n42:        dw 0
spk_n43:        dw 0
spk_n61:        dw 0
section .bss
spk_info:       resb 16                         ; ring address, size, LPIB, rate
spk_db_stack:   resb 512
spk_db_stack_top:
section .text
