; =============================================================================
;  mouse.asm - PS/2 mouse driver talking directly to the 8042 controller
; -----------------------------------------------------------------------------
;  Works with real PS/2 mice and touchpads and with USB mice under the BIOS's
;  "legacy USB" emulation.  IRQ12 (INT 74h) delivers 3-byte packets; motion
;  and button state are accumulated in mouse_dx / mouse_dy / mouse_buttons
;  for the GUI to poll.  Every controller access has a timeout, so a machine
;  without a PS/2 controller just ends up without a mouse.
; =============================================================================

PS2_DATA        equ 0x60
PS2_STATUS      equ 0x64
PS2_CMD         equ 0x64

; ---- low-level controller helpers -------------------------------------------
; ps2_wait_input: wait until the controller can take a byte.  CF=1 on timeout
ps2_wait_input:
        push    cx
        push    ax
        mov     cx, 0xFFFF
.wait:  in      al, PS2_STATUS
        test    al, 0x02
        jz      .ok
        loop    .wait
        stc
        jmp     .done
.ok:    clc
.done:  pop     ax
        pop     cx
        ret

; ps2_wait_output: wait until a byte is available.  CF=1 on timeout
ps2_wait_output:
        push    cx
        push    ax
        mov     cx, 0xFFFF
.wait:  in      al, PS2_STATUS
        test    al, 0x01
        jnz     .ok
        loop    .wait
        stc
        jmp     .done
.ok:    clc
.done:  pop     ax
        pop     cx
        ret

; ps2_wait_output_long: like ps2_wait_output but waits about a second,
;   for slow devices answering a reset.  CF=1 on timeout
ps2_wait_output_long:
        push    ecx
        push    ax
        mov     ecx, 0x00100000
.wait:  in      al, PS2_STATUS
        test    al, 0x01
        jnz     .ok
        out     0x80, al                        ; ~1 us I/O delay
        dec     ecx
        jnz     .wait
        stc
        jmp     .done
.ok:    clc
.done:  pop     ax
        pop     ecx
        ret

; ps2_flush: throw away anything waiting in the output buffer
ps2_flush:
        push    ax
        push    cx
        mov     cx, 64
.next:  in      al, PS2_STATUS
        test    al, 0x01
        jz      .done
        in      al, PS2_DATA
        loop    .next
.done:  pop     cx
        pop     ax
        ret

; ps2_cmd: AL -> command port.  ps2_data: AL -> data port.  CF=1 on timeout
ps2_cmd:
        call    ps2_wait_input
        jc      .done
        out     PS2_CMD, al
.done:  ret

ps2_data:
        call    ps2_wait_input
        jc      .done
        out     PS2_DATA, al
.done:  ret

; ps2_mouse_cmd: send AL to the mouse and wait for its ACK (FAh).  CF=1 if not
ps2_mouse_cmd:
        push    ax
        mov     al, 0xD4                        ; "next byte goes to the mouse"
        call    ps2_cmd
        jc      .fail
        pop     ax
        push    ax
        call    ps2_data
        jc      .fail
        call    ps2_wait_output_long
        jc      .fail
        in      al, PS2_DATA
        cmp     al, 0xFA
        je      .acked
        cmp     al, 0xFE                        ; "resend": try once more
        jne     .fail
        call    ps2_wait_output
        jc      .fail
        in      al, PS2_DATA
        cmp     al, 0xFA
        jne     .fail
.acked: pop     ax
        clc
        ret
.fail:  pop     ax
        stc
        ret

; -----------------------------------------------------------------------------
; mouse_init: enable the auxiliary device, start reporting, hook IRQ12.
;   CF=1 if no mouse responds (the keyboard is left untouched).
; -----------------------------------------------------------------------------
mouse_init:
        pusha
        push    es
        mov     byte [mouse_ok], 0
        mov     word [mouse_dx], 0
        mov     word [mouse_dy], 0
        mov     byte [mouse_buttons], 0
        mov     byte [ms_phase], 0
        mov     byte [mouse_bios], 0
        cli
        call    ps2_flush
        cmp     byte [mouse_force_bios], 0      ; WIN BIOSMOUSE: skip the probe
        je      .probe
        mov     byte [mouse_fail_step], 4
        jmp     .fail
.probe: mov     byte [mouse_fail_step], 1       ; controller not responding
        mov     al, 0xA8                        ; enable the auxiliary port
        call    ps2_cmd
        jc      .fail
        mov     al, 0x20                        ; read the controller command byte
        call    ps2_cmd
        jc      .fail
        mov     byte [mouse_fail_step], 2       ; no command byte returned
        call    ps2_wait_output
        jc      .fail
        in      al, PS2_DATA
        or      al, 0x02                        ; enable IRQ12
        and     al, 0xDF                        ; enable the mouse clock
        mov     ah, al
        mov     byte [mouse_fail_step], 3       ; command byte write failed
        mov     al, 0x60                        ; write the command byte back
        call    ps2_cmd
        jc      .fail
        mov     al, ah
        call    ps2_data
        jc      .fail
        call    ps2_flush
        mov     byte [mouse_fail_step], 4       ; mouse did not acknowledge reset
        mov     al, 0xFF                        ; mouse: reset
        call    ps2_mouse_cmd
        jc      .fail
        mov     byte [mouse_fail_step], 5       ; no self-test result after reset
        call    ps2_wait_output_long
        jc      .fail
        in      al, PS2_DATA                    ; AAh = self-test passed
        cmp     al, 0xAA
        jne     .fail
        call    ps2_wait_output_long            ; device id (00h), optional
        jc      .no_id
        in      al, PS2_DATA
.no_id: mov     byte [mouse_fail_step], 6       ; no ACK to "set defaults"
        mov     al, 0xF6                        ; mouse: restore defaults
        call    ps2_mouse_cmd
        jc      .fail
        mov     byte [mouse_fail_step], 7       ; no ACK to "enable reporting"
        mov     al, 0xF4                        ; mouse: enable data reporting
        call    ps2_mouse_cmd
        jc      .fail
        mov     byte [mouse_fail_step], 0
.hook:  ; hook INT 74h (IRQ12) and unmask IRQ12 + the cascade (IRQ2)
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0x74*4]
        mov     [old_int74_off], ax
        mov     ax, [es:0x74*4+2]
        mov     [old_int74_seg], ax
        mov     word [es:0x74*4], irq12_handler
        mov     [es:0x74*4+2], cs
        in      al, 0xA1
        mov     [old_mask_a1], al
        and     al, 0xEF
        out     0xA1, al
        in      al, 0x21
        mov     [old_mask_21], al
        and     al, 0xFB
        out     0x21, al
        mov     byte [mouse_ok], 1
        sti
        pop     es
        popa
        clc
        ret
.fail:  ; No device answered on the port (codes 4/5): the mouse may be a USB
        ; one that the BIOS only emulates after being asked.  Let the BIOS
        ; initialise and enable it, then take the interrupt ourselves.
        cmp     byte [mouse_fail_step], 4
        jb      .give_up
        cmp     byte [mouse_fail_step], 5
        ja      .give_up
        call    mouse_bios_assist
        jnc     .hook
.give_up:
        sti
        pop     es
        popa
        stc
        ret

; mouse_bios_assist: INT 15h C2xxh initialise + enable with a do-nothing
;   callback.  CF=1 if the BIOS has no mouse services or refuses.
mouse_bios_assist:
        push    es
        push    ax
        push    bx
        sti
        mov     byte [mouse_fail_step], 8       ; BIOS mouse init failed
        mov     ax, 0xC205
        mov     bh, 3                           ; 3-byte packets
        int     0x15
        jc      .fail
        mov     ax, 0xC207
        mov     bx, bios_dummy_handler
        push    cs
        pop     es
        int     0x15
        jc      .fail
        mov     byte [mouse_fail_step], 9       ; BIOS refused to enable
        mov     ax, 0xC200
        mov     bh, 1
        int     0x15
        jc      .fail
        cli
        mov     byte [mouse_bios], 1
        mov     byte [mouse_fail_step], 0
        pop     bx
        pop     ax
        pop     es
        clc
        ret
.fail:  cli
        pop     bx
        pop     ax
        pop     es
        stc
        ret

bios_dummy_handler:
        retf

; -----------------------------------------------------------------------------
; mouse_stop: stop reporting and restore the interrupt vector and masks
; -----------------------------------------------------------------------------
mouse_stop:
        pusha
        push    es
        cmp     byte [mouse_ok], 0
        je      .done
        cli
        mov     al, 0xF5                        ; mouse: disable data reporting
        call    ps2_mouse_cmd
        xor     ax, ax
        mov     es, ax
        mov     ax, [old_int74_off]
        mov     [es:0x74*4], ax
        mov     ax, [old_int74_seg]
        mov     [es:0x74*4+2], ax
        mov     al, [old_mask_a1]
        out     0xA1, al
        mov     al, [old_mask_21]
        out     0x21, al
        call    ps2_flush
        sti
        cmp     byte [mouse_bios], 0
        je      .off
        mov     ax, 0xC200                      ; BIOS: disable the device
        xor     bh, bh
        int     0x15
        mov     ax, 0xC207                      ; and forget the callback
        xor     bx, bx
        mov     es, bx
        int     0x15
        mov     byte [mouse_bios], 0
.off:   mov     byte [mouse_ok], 0
.done:  pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; irq12_handler: one byte of a mouse packet has arrived
; -----------------------------------------------------------------------------
irq12_handler:
        push    ax
        push    bx
        push    ds
        mov     ax, cs
        mov     ds, ax
        in      al, PS2_STATUS
        test    al, 0x01                        ; anything there?
        jz      .eoi
        cmp     byte [mouse_bios], 0            ; BIOS-emulated USB mice do not
        jne     .read                           ; always flag the byte as "aux"
        test    al, 0x20                        ; from the mouse (not the keyboard)?
        jz      .eoi
.read:  in      al, PS2_DATA
        movzx   bx, byte [ms_phase]
        test    bx, bx
        jnz     .store
        test    al, 0x08                        ; first byte always has bit 3 set
        jz      .eoi                            ; out of sync: drop it
.store: mov     [ms_packet+bx], al
        inc     bx
        cmp     bx, 3
        jb      .phase
        call    ms_process
        xor     bx, bx
.phase: mov     [ms_phase], bl
.eoi:   mov     al, 0x20
        out     0xA0, al                        ; EOI to the slave PIC
        out     0x20, al                        ; and the master
        pop     ds
        pop     bx
        pop     ax
        iret

; ms_process: decode ms_packet into the shared motion/button variables
ms_process:
        push    ax
        push    cx
        mov     al, [ms_packet]
        test    al, 0xC0                        ; X/Y overflow: ignore the packet
        jnz     .done
        mov     ah, al
        and     ah, 0x07
        mov     [mouse_buttons], ah
        movzx   cx, byte [ms_packet+1]
        test    al, 0x10
        jz      .x_pos
        sub     cx, 256
.x_pos: add     [mouse_dx], cx
        movzx   cx, byte [ms_packet+2]
        test    al, 0x20
        jz      .y_pos
        sub     cx, 256
.y_pos: sub     [mouse_dy], cx                  ; PS/2 Y grows upwards
.done:  pop     cx
        pop     ax
        ret

section .data
mouse_fail_step: db 0                          ; 0 = ok / not tried, else step
mouse_bios:     db 0                            ; 1 = device enabled via INT 15h
mouse_force_bios: db 0                          ; 1 = always use the BIOS path
old_int74_off:  dw 0
old_int74_seg:  dw 0
old_mask_a1:    db 0
old_mask_21:    db 0
ms_phase:       db 0
ms_packet:      db 0, 0, 0
section .text
