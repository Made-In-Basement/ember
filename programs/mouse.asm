; =============================================================================
;  MOUSE.COM - what does the mouse port actually say?
; -----------------------------------------------------------------------------
;  A laptop's "PS/2 mouse" is usually a USB one that the firmware is
;  impersonating, and every firmware does it a little differently.  Rather
;  than guess, this asks the port the same questions the desktop's driver
;  asks, and writes down every answer with the time it took:
;
;    1. the probe: enable the port, reset the mouse, read what comes back
;    2. six seconds of raw bytes as they arrive, with which interrupt
;       brought each and what the status port said about it
;    3. the same again through the firmware's own INT 15h mouse services,
;       to see whether its driver makes sense of the device when ours
;       does not
;
;  Everything goes to the screen and to C:\MOUSE.TXT.
; =============================================================================

[BITS 16]
[ORG 0x0100]

PS2_DATA        equ 0x60
PS2_STATUS      equ 0x64
TICKS           equ 0x046C                      ; the BIOS clock, 18.2 a second
TRACE_MAX       equ 400
WATCH_TICKS     equ 110                         ; about six seconds
CB_MAX          equ 48

start:
        mov     di, bss_start                   ; a .COM starts with whatever
        mov     cx, bss_end - bss_start         ;  was in memory before it
        xor     al, al
        cld
        rep     stosb
        xor     ax, ax
        mov     fs, ax                          ; the BIOS clock lives in segment 0

        mov     si, msg_head
        call    puts

        ; ============================================================ 1. probe
        mov     si, msg_probe
        call    puts
        mov     bx, 5                           ; let the Enter key come up first
        call    wait_ticks
        ; The mouse's replies raise IRQ12, and the firmware's own handler
        ; would take them before we could look.  Keep that interrupt masked
        ; while we ask; the keyboard's is left alone so typing still works.
        in      al, 0xA1
        mov     [oldmask_a1], al
        or      al, 0x10
        out     0xA1, al

        mov     al, 0xA8                        ; switch the mouse port on
        call    ps2_cmd
        ; Read the controller's command byte.  A key going up while this
        ; runs leaves its scancode in the same buffer, and the first time
        ; that happened the release of the Enter key was written back as
        ; the configuration, which switched the keyboard off.  So: nobody
        ; else reads the port meanwhile, and a byte with bit 7 set is a
        ; scancode, not a setting.
        mov     cx, 4
.ask:   cli
        call    ps2_flush
        mov     al, 0x20
        call    ps2_cmd
        mov     bx, 2
        call    wait_out
        jc      .no_cmd_byte
        in      al, PS2_DATA
        sti
        test    al, 0x80
        jz      .have_cmd
        loop    .ask
        jmp     .no_cmd_byte
.have_cmd:
        mov     [cmd_byte], al
        mov     si, msg_cmdbyte
        call    puts
        call    print_hex8
        call    crlf
        or      al, 0x02                        ; let the mouse interrupt
        and     al, 0xDF                        ; and stop ignoring its clock
        mov     ah, al
        mov     al, 0x60
        call    ps2_cmd
        mov     al, ah
        call    ps2_data
        jmp     .reset
.no_cmd_byte:
        sti
        mov     si, msg_nocmd
        call    puts
.reset:
        call    ps2_flush
        mov     si, msg_reset
        call    puts
        mov     al, 0xFF                        ; reset
        mov     bx, 6                           ; a third of a second for the ack
        call    aux_cmd_timed                   ; prints what came back
        mov     si, msg_selftest
        call    puts
        mov     bx, 20                          ; a second for the self-test
        call    read_timed
        mov     si, msg_ident
        call    puts
        mov     bx, 6
        call    read_timed
        mov     si, msg_defaults
        call    puts
        mov     al, 0xF6
        mov     bx, 6
        call    aux_cmd_timed
        mov     si, msg_enable
        call    puts
        mov     al, 0xF4
        mov     bx, 6
        call    aux_cmd_timed

        call    write_log                       ; in case what follows hangs

        ; ============================================================ 2. watch
        mov     si, msg_watch
        call    puts
        call    ps2_flush
        mov     al, [oldmask_a1]
        out     0xA1, al
        cli
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0x74*4]
        mov     [old74], ax
        mov     ax, [es:0x74*4+2]
        mov     [old74+2], ax
        mov     ax, [es:0x09*4]
        mov     [old09], ax
        mov     ax, [es:0x09*4+2]
        mov     [old09+2], ax
        mov     word [es:0x74*4], irq12_hook
        mov     [es:0x74*4+2], cs
        mov     word [es:0x09*4], irq1_hook
        mov     [es:0x09*4+2], cs
        in      al, 0xA1
        mov     [oldmask_a1], al
        and     al, 0xEF                        ; IRQ12
        out     0xA1, al
        in      al, 0x21
        mov     [oldmask_21], al
        and     al, 0xFB                        ; and the cascade
        out     0x21, al
        mov     eax, [fs:TICKS]
        mov     [t_start], eax
        sti
        mov     bx, WATCH_TICKS
        call    wait_ticks
        cli
        mov     al, 0xF5                        ; stop reporting
        call    aux_send
        mov     ax, [old74]
        mov     [es:0x74*4], ax
        mov     ax, [old74+2]
        mov     [es:0x74*4+2], ax
        mov     ax, [old09]
        mov     [es:0x09*4], ax
        mov     ax, [old09+2]
        mov     [es:0x09*4+2], ax
        mov     al, [oldmask_a1]
        out     0xA1, al
        mov     al, [oldmask_21]
        out     0x21, al
        sti
        call    ps2_flush
        call    show_trace
        call    write_log                       ; the firmware test may freeze

        ; ============================================================ 3. BIOS
        mov     si, msg_bios
        call    puts
        mov     ax, 0xC204                      ; what does the firmware think it is?
        int     0x15
        jc      .no_type
        mov     si, msg_type
        call    puts
        mov     al, bh
        call    print_hex8
        call    crlf
        jmp     .init
.no_type:
        mov     si, msg_notype
        call    puts
.init:  mov     ax, 0xC205
        mov     bh, 3                           ; three-byte packets
        int     0x15
        mov     si, msg_c205
        call    puts
        call    print_cf_ah
        mov     ax, 0xC207
        push    cs
        pop     es
        mov     bx, bios_callback
        int     0x15
        mov     si, msg_c207
        call    puts
        call    print_cf_ah
        mov     ax, 0xC200
        mov     bh, 1
        int     0x15
        mov     si, msg_c200
        call    puts
        call    print_cf_ah
        jc      .bios_done
        mov     si, msg_watch2
        call    puts
        mov     bx, WATCH_TICKS
        call    wait_ticks
        mov     ax, 0xC200
        xor     bh, bh
        int     0x15
        mov     ax, 0xC207
        xor     bx, bx
        mov     es, bx
        int     0x15
        call    show_callbacks
.bios_done:
        ; No file write after this point: once the firmware's mouse
        ; services have been on, writing to the stick froze one machine.
        ; The file already holds stages 1 and 2; this stage is for the eye.
        mov     si, msg_done_screen
        call    puts_screen
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------- hooks
; One byte per interrupt, recorded with its status and the time.
irq12_hook:
        push    ax
        push    bx
        push    ds
        push    fs
        push    cs
        pop     ds
        xor     ax, ax
        mov     fs, ax
        in      al, PS2_STATUS
        mov     ah, al
        test    al, 0x01
        jz      .eoi
        in      al, PS2_DATA
        mov     bl, 12
        call    trace_add
.eoi:   mov     al, 0x20
        out     0xA0, al
        out     0x20, al
        pop     fs
        pop     ds
        pop     bx
        pop     ax
        iret

; The keyboard's interrupt: a byte flagged as the mouse's is taken and
; recorded; anything else is left for the BIOS, so typing still works.
irq1_hook:
        push    ax
        push    bx
        push    ds
        push    fs
        push    cs
        pop     ds
        xor     ax, ax
        mov     fs, ax
        in      al, PS2_STATUS
        mov     ah, al
        and     al, 0x21
        cmp     al, 0x21
        jne     .chain
        in      al, PS2_DATA
        mov     bl, 1
        call    trace_add
        mov     al, 0x20
        out     0x20, al
        pop     fs
        pop     ds
        pop     bx
        pop     ax
        iret
.chain: pop     fs
        pop     ds
        pop     bx
        pop     ax
        jmp     far [cs:old09]

; AH = status, AL = byte, BL = which interrupt
trace_add:
        push    di
        push    eax
        mov     di, [trace_n]
        cmp     di, TRACE_MAX
        jae     .full
        shl     di, 3
        add     di, trace
        mov     [di], bl
        mov     [di+1], ah
        mov     [di+2], al
        mov     eax, [fs:TICKS]
        sub     eax, [t_start]
        mov     [di+4], eax
        inc     word [trace_n]
.full:  pop     eax
        pop     di
        ret

; The firmware's driver hands us status, X, Y, Z on the stack.
bios_callback:
        push    bp
        mov     bp, sp
        push    ax
        push    di
        mov     di, [cs:cb_n]
        cmp     di, CB_MAX
        jae     .full
        shl     di, 2
        add     di, cb_log
        mov     ax, [bp+12]                     ; status
        mov     [cs:di], al
        mov     ax, [bp+10]                     ; X
        mov     [cs:di+1], al
        mov     ax, [bp+8]                      ; Y
        mov     [cs:di+2], al
        inc     word [cs:cb_n]
.full:  inc     word [cs:cb_total]
        pop     di
        pop     ax
        pop     bp
        retf

; ---------------------------------------------------------------- reports
show_trace:
        mov     si, msg_trace1
        call    puts
        mov     ax, [trace_n]
        call    print_dec
        mov     si, msg_trace2
        call    puts
        xor     bp, bp
.next:  cmp     bp, [trace_n]
        jae     .done
        cmp     bp, 160                         ; enough to see the pattern
        jae     .more
        mov     di, bp
        shl     di, 3
        add     di, trace
        mov     si, msg_irq
        call    puts
        movzx   ax, byte [di]
        call    print_dec
        mov     si, msg_status
        call    puts
        mov     al, [di+1]
        call    print_hex8
        mov     si, msg_byte
        call    puts
        mov     al, [di+2]
        call    print_hex8
        mov     si, msg_at
        call    puts
        mov     eax, [di+4]
        mov     edx, 55                         ; ticks to milliseconds, near enough
        mul     edx
        call    print_dec32
        mov     si, msg_ms
        call    puts
        call    crlf
        inc     bp
        jmp     .next
.more:  mov     si, msg_andmore
        call    puts
.done:  ret

show_callbacks:
        mov     si, msg_cb1
        call    puts
        mov     ax, [cb_total]
        call    print_dec
        mov     si, msg_cb2
        call    puts
        xor     bp, bp
.next:  cmp     bp, [cb_n]
        jae     .done
        mov     di, bp
        shl     di, 2
        add     di, cb_log
        mov     si, msg_cbrow
        call    puts
        mov     al, [di]
        call    print_hex8
        mov     si, msg_sp
        call    puts
        mov     al, [di+1]
        call    print_signed8
        mov     si, msg_sp
        call    puts
        mov     al, [di+2]
        call    print_signed8
        call    crlf
        inc     bp
        jmp     .next
.done:  ret

; CF and AH from an INT 15h call, as words
print_cf_ah:
        pushf
        jc      .failed
        mov     si, msg_ok
        call    puts
        popf
        ret
.failed:
        mov     si, msg_failed
        call    puts
        mov     al, ah
        call    print_hex8
        call    crlf
        popf
        ret

; ---------------------------------------------------------------- the port
; ps2_cmd: AL to the controller (port 64h)
ps2_cmd:
        call    wait_in
        out     PS2_STATUS, al
        ret

; ps2_data: AL to the data port
ps2_data:
        call    wait_in
        out     PS2_DATA, al
        ret

; aux_send: AL to the mouse, no answer wanted
aux_send:
        push    ax
        mov     al, 0xD4
        call    ps2_cmd
        pop     ax
        call    ps2_data
        ret

; aux_cmd_timed: AL to the mouse, then print what came back within BX ticks
aux_cmd_timed:
        call    aux_send
        ; fall into read_timed
; read_timed: print the next byte and how long it took, or "nothing"
read_timed:
        push    bx
        mov     eax, [fs:TICKS]
        mov     [t_cmd], eax
        call    wait_out
        jc      .nothing
        in      al, PS2_STATUS
        mov     ah, al
        in      al, PS2_DATA
        push    ax
        mov     si, msg_got
        call    puts
        pop     ax
        push    ax
        call    print_hex8
        mov     si, msg_stat
        call    puts
        pop     ax
        mov     al, ah
        call    print_hex8
        mov     si, msg_after
        call    puts
        mov     eax, [fs:TICKS]
        sub     eax, [t_cmd]
        mov     edx, 55
        mul     edx
        call    print_dec32
        mov     si, msg_ms
        call    puts
        call    crlf
        pop     bx
        ret
.nothing:
        mov     si, msg_nothing
        call    puts
        pop     bx
        ret

; wait_in: until the controller can take a byte (a bounded spin)
wait_in:
        push    cx
        push    ax
        mov     cx, 0xFFFF
.spin:  in      al, PS2_STATUS
        test    al, 0x02
        jz      .ok
        loop    .spin
.ok:    pop     ax
        pop     cx
        ret

; wait_out: until a byte is waiting, for at most BX ticks.  CF=1 if not.
wait_out:
        push    eax
        push    ebx
        movzx   ebx, bx
        mov     eax, [fs:TICKS]
        add     ebx, eax
.spin:  in      al, PS2_STATUS
        test    al, 0x01
        jnz     .ok
        mov     eax, [fs:TICKS]
        cmp     eax, ebx
        jb      .spin
        stc
        jmp     .out
.ok:    clc
.out:   pop     ebx
        pop     eax
        ret

; wait_ticks: BX ticks of the BIOS clock
wait_ticks:
        push    eax
        push    ebx
        movzx   ebx, bx
        add     ebx, [fs:TICKS]
.spin:  hlt
        mov     eax, [fs:TICKS]
        cmp     eax, ebx
        jb      .spin
        pop     ebx
        pop     eax
        ret

ps2_flush:
        push    ax
        push    cx
        mov     cx, 64
.loop:  in      al, PS2_STATUS
        test    al, 0x01
        jz      .done
        in      al, PS2_DATA
        loop    .loop
.done:  pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------- the file
write_log:
        mov     dx, log_name
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .failed
        mov     bx, ax
        mov     cx, [log_len]
        mov     dx, log_buf
        mov     ah, 0x40
        int     0x21
        mov     ah, 0x3E
        int     0x21
        mov     si, msg_saved
        call    puts_screen
        ret
.failed:
        mov     si, msg_nosave
        call    puts_screen
        ret

; ---------------------------------------------------------------- printing
puts:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:  pop     ax
        ret

puts_screen:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc_screen
        jmp     .loop
.done:  pop     ax
        ret

putc:
        call    putc_screen
        push    bx
        mov     bx, [log_len]
        cmp     bx, LOG_MAX - 1
        jae     .full
        mov     [log_buf+bx], al
        inc     word [log_len]
.full:  pop     bx
        ret

putc_screen:
        push    ax
        push    bx
        mov     ah, 0x0E
        xor     bx, bx
        int     0x10
        pop     bx
        pop     ax
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

; AL as a signed number
print_signed8:
        push    ax
        test    al, al
        jns     .pos
        neg     al
        push    ax
        mov     al, '-'
        call    putc
        pop     ax
.pos:   movzx   ax, al
        call    print_dec
        pop     ax
        ret

print_dec:
        movzx   eax, ax
        ; fall into print_dec32
print_dec32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     cx, cx
        test    eax, eax
        jnz     .split
        mov     al, '0'
        call    putc
        jmp     .out
.split: xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .split
.emit:  pop     ax
        add     al, '0'
        call    putc
        loop    .emit
.out:   pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

print_hex8:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .digit
        mov     al, cl
        and     al, 0x0F
        call    .digit
        pop     cx
        pop     ax
        ret
.digit: and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    putc
        ret

msg_head:     db "What the mouse port says", 13, 10
              db "========================", 13, 10, 0
msg_probe:    db "1. Asking the controller and the mouse directly", 13, 10, 0
msg_cmdbyte:  db "   controller command byte: ", 0
msg_nocmd:    db "   the controller gave no command byte", 13, 10, 0
msg_reset:    db "   reset (FF):      ", 0
msg_selftest: db "   self-test:       ", 0
msg_ident:    db "   identity:        ", 0
msg_defaults: db "   defaults (F6):   ", 0
msg_enable:   db "   enable (F4):     ", 0
msg_got:      db "got ", 0
msg_stat:     db " (status ", 0
msg_after:    db ") after ", 0
msg_ms:       db " ms", 0
msg_nothing:  db "nothing came back", 13, 10, 0
msg_watch:    db "2. Move the mouse and click for six seconds...", 13, 10, 0
msg_trace1:   db "   bytes seen: ", 0
msg_trace2:   db 13, 10, 0
msg_irq:      db "   irq", 0
msg_status:   db "  status ", 0
msg_byte:     db "  byte ", 0
msg_at:       db "  at +", 0
msg_andmore:  db "   ... and more", 13, 10, 0
msg_bios:     db "3. Through the firmware's own mouse services (this may freeze the machine;", 13, 10
              db "   the file is already saved)", 13, 10, 0
msg_type:     db "   device type: ", 0
msg_notype:   db "   the firmware reports no device type", 13, 10, 0
msg_c205:     db "   initialise: ", 0
msg_c207:     db "   set handler: ", 0
msg_c200:     db "   enable:     ", 0
msg_ok:       db "ok", 13, 10, 0
msg_failed:   db "failed, code ", 0
msg_watch2:   db "   Move the mouse and click again for six seconds...", 13, 10, 0
msg_cb1:      db "   packets from the firmware: ", 0
msg_cb2:      db 13, 10, "   status dx dy", 13, 10, 0
msg_cbrow:    db "   ", 0
msg_sp:       db " ", 0
msg_done:     db "Done.", 13, 10, 0
msg_done_screen: db "Done.  Stages 1 and 2 are in C:\MOUSE.TXT; photograph the screen for stage 3.", 13, 10, 0
msg_saved:    db "Written to C:\MOUSE.TXT", 13, 10, 0
msg_nosave:   db "Could not write C:\MOUSE.TXT", 13, 10, 0
log_name:     db "\MOUSE.TXT", 0

LOG_MAX     equ 16384

section .bss
bss_start:
cmd_byte:   resb 1
old74:      resw 2
old09:      resw 2
oldmask_a1: resb 1
oldmask_21: resb 1
t_start:    resd 1
t_cmd:      resd 1
trace_n:    resw 1
trace:      resb TRACE_MAX * 8
cb_n:       resw 1
cb_total:   resw 1
cb_log:     resb CB_MAX * 4
log_len:    resw 1
log_buf:    resb LOG_MAX
bss_end:
