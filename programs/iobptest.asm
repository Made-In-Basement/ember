; =============================================================================
;  IOBPTEST.COM - how does this machine behave with I/O breakpoints armed?
; -----------------------------------------------------------------------------
;  The SPEAKER bridge watches ports 42h, 43h and 61h with the processor's
;  hardware I/O breakpoints.  On one laptop that locked the machine up, so
;  this probe measures what happens, step by step, and can never hang:
;  every loop is bounded and the trap handler disarms itself if it is
;  entered more times than any sane program would need.
;
;  It answers three questions:
;    1. are I/O breakpoints delivered at all, and after the instruction?
;    2. does anything OTHER than this program touch those ports?  (traps
;       counted during a busy loop that does no I/O of its own)
;    3. does a 1984-style tone sequence survive with them armed?
; =============================================================================

[BITS 16]
[ORG 0x0100]

TRAP_CAP        equ 20000                       ; disarm past this many

start:
        mov     si, msg_hello
        call    puts

; ---------------------------------------------------------------- step 1
        mov     si, msg_step1
        call    puts
        call    arm_one                         ; DR0 on port 61h only
        in      al, 0x61
        out     0x61, al
        call    disarm
        mov     si, msg_traps
        call    puts
        mov     ax, [count]
        call    print_num
        mov     si, msg_expect2
        call    puts
        cmp     word [count], 2
        je      .step1_ok
        mov     si, msg_bad
        call    puts
        jmp     finish
.step1_ok:
        mov     si, msg_ok
        call    puts

; ---------------------------------------------------------------- step 2
        mov     si, msg_step2
        call    puts
        call    ticks_now
        mov     [t_base], ax
        call    busy_loop
        call    ticks_now
        sub     ax, [t_base]
        mov     [d_idle], ax
        mov     si, msg_idle
        call    puts
        mov     ax, [d_idle]
        call    print_num
        call    crlf
        ; each watched port on its own, so a flood can be blamed on one
        mov     bx, ports
.each:  mov     ax, [bx]
        or      ax, ax
        jz      .each_done
        push    bx
        mov     word [count], 0
        call    arm_one_ax                      ; AX = the port to watch
        call    ticks_now
        mov     [t_base], ax
        call    busy_loop                       ; this loop does no I/O at all
        call    ticks_now
        sub     ax, [t_base]
        mov     [d_armed], ax
        call    disarm
        pop     bx
        mov     si, msg_port
        call    puts
        mov     ax, [bx]
        call    print_hex16
        mov     si, msg_pt
        call    puts
        mov     ax, [count]
        call    print_num
        mov     si, msg_pticks
        call    puts
        mov     ax, [d_armed]
        call    print_num
        call    crlf
        add     bx, 2
        jmp     .each
.each_done:

; ---------------------------------------------------------------- step 3
        mov     si, msg_step3
        call    puts
        mov     word [count], 0
        call    arm_three
        mov     bx, notes
.next:  mov     ax, [bx]
        or      ax, ax
        jz      .tones_done
        call    tone
        call    short_delay
        call    tone_off
        add     bx, 2
        jmp     .next
.tones_done:
        call    tone_off
        call    disarm
        mov     si, msg_tonetraps
        call    puts
        mov     ax, [count]
        call    print_num
        mov     si, msg_expect49
        call    puts

finish:
        mov     si, msg_capped
        cmp     byte [capped], 0
        je      .not_capped
        call    puts
.not_capped:
        mov     si, msg_survived
        call    puts
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------- breakpoints
arm_one:
        pushad
        call    hook
        mov     eax, 0x61
        mov     dr0, eax
        mov     eax, 0x00020001                 ; L0, R/W0 = I/O, 1 byte
        mov     dr7, eax
        sti
        popad
        ret

arm_three:
        pushad
        call    hook
        mov     eax, 0x42
        mov     dr0, eax
        mov     eax, 0x43
        mov     dr1, eax
        mov     eax, 0x61
        mov     dr2, eax
        mov     eax, 0x02220015                 ; L0-L2, all I/O, 1 byte
        mov     dr7, eax
        sti
        popad
        ret

arm_one_ax:                                     ; AX = port
        push    eax
        pushad
        call    hook
        popad
        movzx   eax, ax
        mov     dr0, eax
        mov     eax, 0x00020001
        mov     dr7, eax
        sti
        pop     eax
        ret

hook:                                           ; called with registers saved
        xor     ax, ax
        mov     es, ax
        cli
        mov     eax, [es:1*4]
        mov     [old_int1], eax
        mov     word [es:1*4], db_handler
        mov     [es:1*4+2], cs
        mov     eax, cr4
        or      eax, 0x08                       ; CR4.DE
        mov     cr4, eax
        xor     eax, eax
        mov     dr6, eax
        mov     byte [capped], 0
        ret

disarm:
        pushad
        cli
        xor     eax, eax
        mov     dr7, eax
        mov     dr6, eax
        xor     ax, ax
        mov     es, ax
        mov     eax, [old_int1]
        or      eax, eax
        jz      .no_vec
        mov     [es:1*4], eax
.no_vec:
        sti
        popad
        ret

; the trap, with a cap so a storm cannot lock the machine
db_handler:
        push    ax
        push    ds
        mov     ax, cs
        mov     ds, ax
        inc     word [count]
        cmp     word [count], TRAP_CAP
        jb      .keep
        mov     byte [capped], 1
        xor     eax, eax
        mov     dr7, eax                        ; stand down, right now
.keep:  xor     eax, eax
        mov     dr6, eax
        pop     ds
        pop     ax
        iret

; ---------------------------------------------------------------- helpers
; busy_loop: about a fifth of a second of arithmetic, no I/O, bounded
busy_loop:
        push    ecx
        mov     ecx, 60000000
.spin:  dec     ecx
        jnz     .spin
        pop     ecx
        ret

; short_delay: bounded, and does not depend on the timer tick advancing
short_delay:
        push    ecx
        mov     ecx, 30000000
.spin:  dec     ecx
        jnz     .spin
        pop     ecx
        ret

ticks_now:                                      ; AX = the BIOS tick, low word
        push    es
        push    bx
        xor     bx, bx
        mov     es, bx
        mov     ax, [es:0x046C]
        pop     bx
        pop     es
        ret

tone:                                           ; AX = frequency
        push    ax
        push    bx
        push    dx
        mov     bx, ax
        mov     al, 0xB6
        out     0x43, al
        mov     dx, 0x0012
        mov     ax, 0x34DC
        div     bx
        out     0x42, al
        mov     al, ah
        out     0x42, al
        in      al, 0x61
        or      al, 0x03
        out     0x61, al
        pop     dx
        pop     bx
        pop     ax
        ret

tone_off:
        push    ax
        in      al, 0x61
        and     al, 0xFC
        out     0x61, al
        pop     ax
        ret

puts:
        push    ax
        push    si
.ch:    lodsb
        or      al, al
        jz      .end
        mov     ah, 0x0E
        int     0x10
        jmp     .ch
.end:   pop     si
        pop     ax
        ret

crlf:   push    ax
        mov     ax, 0x0E0D
        int     0x10
        mov     ax, 0x0E0A
        int     0x10
        pop     ax
        ret

print_hex16:                                    ; AX as four hex digits
        push    ax
        push    cx
        mov     cx, 4
.d:     rol     ax, 4
        push    ax
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     ah, 0x0E
        int     0x10
        pop     ax
        loop    .d
        pop     cx
        pop     ax
        ret

print_num:                                      ; AX in decimal
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
.div:   xor     dx, dx
        div     bx
        push    dx
        inc     cx
        or      ax, ax
        jnz     .div
.out:   pop     ax
        add     al, '0'
        mov     ah, 0x0E
        int     0x10
        loop    .out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

msg_hello:  db "IOBPTEST: measuring hardware I/O breakpoints.", 13, 10
            db "Nothing here can hang: every loop is counted out.", 13, 10, 13, 10, 0
msg_step1:  db "1. one breakpoint on port 61h, one read and one write", 13, 10, 0
msg_traps:  db "   traps taken: ", 0
msg_expect2: db " (2 expected)  ", 0
msg_ok:     db "OK", 13, 10, 13, 10, 0
msg_bad:    db "WRONG - the bridge cannot work here.", 13, 10, 0
msg_step2:  db "2. busy loop with ports 42h/43h/61h watched, doing no I/O", 13, 10, 0
msg_idle:   db "   timer ticks in that loop, nothing watched: ", 0
msg_port:   db "   port ", 0
msg_pt:     db "h: traps from other code ", 0
msg_pticks: db ", timer ticks ", 0
msg_step3:  db 13, 10, "3. a 1984-style tone sequence with them watched", 13, 10, 0
msg_tonetraps: db "   traps taken: ", 0
msg_expect49: db " (about 51 expected)", 13, 10, 0
msg_capped: db 13, 10, "*** the trap cap was hit: something floods these ports ***", 13, 10, 0
msg_survived: db 13, 10, "Finished without locking up.", 13, 10, 0

count:      dw 0
c_idle:     dw 0
t_base:     dw 0
d_idle:     dw 0
d_armed:    dw 0
capped:     db 0
            align 2
notes:      dw 262, 330, 392, 523, 392, 330, 262, 0
ports:      dw 0x42, 0x43, 0x61, 0
old_int1:   dd 0
