; =============================================================================
;  PCI.COM - list what is on the PCI bus
; -----------------------------------------------------------------------------
;  Written to answer one question: how is this laptop's touchpad attached?
;  A pad that speaks PS/2 shows up on the keyboard controller and needs no
;  driver of its own; one that speaks I2C hangs off a controller that
;  appears here, and that is a different and much larger job.  Either way,
;  the machine has to be asked before anything is written.
;
;  Every device is listed with its vendor and device number, what kind of
;  thing it claims to be, and the address of its first region.  Anything in
;  class 0Ch (serial bus) is worth a second look; 0C80h in particular is
;  what Intel's low-power I2C controllers report themselves as.
; =============================================================================

[BITS 16]
[ORG 0x0100]

PCI_ADDR        equ 0x0CF8
PCI_DATA        equ 0x0CFC

start:
        mov     si, msg_head
        call    puts
        mov     word [lines], 0

        xor     bx, bx                          ; BX = bus:device:function
.next:
        mov     al, 0                           ; register 0: vendor, device
        call    read_cfg                        ; -> EAX
        cmp     ax, 0xFFFF
        je      .skip
        push    eax
        call    show_device
        pop     eax
.skip:
        ; a function other than 0 exists only on a multi-function device
        test    bl, 0x07
        jnz     .advance
        mov     al, 0x0C                        ; header type is at 0Eh
        call    read_cfg
        shr     eax, 16
        test    al, 0x80                        ; the multi-function bit
        jnz     .advance
        add     bx, 7                           ; skip its other functions
.advance:
        inc     bx
        cmp     bx, 0x0800                      ; 8 buses is plenty here
        jb      .next

        mov     si, msg_done
        call    puts
        call    write_log
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------- the file
; Everything that went to the screen is kept in a buffer and written to
; \PCI.TXT, so the answer can be carried off the machine on the stick
; instead of being copied down by hand.
write_log:
        mov     dx, log_name
        xor     cx, cx
        mov     ah, 0x3C                        ; create
        int     0x21
        jc      .failed
        mov     bx, ax
        mov     cx, [log_len]
        mov     dx, log_buf
        mov     ah, 0x40                        ; write
        int     0x21
        mov     ah, 0x3E                        ; close
        int     0x21
        mov     si, msg_saved
        call    puts_screen
        ret
.failed:
        mov     si, msg_nosave
        call    puts_screen
        ret

; ---------------------------------------------------------------- one device
; EAX = vendor and device, BX = where it lives
show_device:
        pushad
        mov     [dev_id], eax

        mov     al, bh                          ; bus
        call    print_hex8
        mov     al, ':'
        call    putc
        mov     al, bl
        shr     al, 3                           ; device
        call    print_hex8
        mov     al, '.'
        call    putc
        mov     al, bl
        and     al, 7                           ; function
        add     al, '0'
        call    putc
        mov     al, ' '
        call    putc

        mov     ax, [dev_id]                    ; vendor
        call    print_hex16
        mov     al, ':'
        call    putc
        mov     ax, [dev_id+2]                  ; device
        call    print_hex16

        mov     si, msg_sp
        call    puts
        mov     al, 0x08                        ; revision and class
        call    read_cfg
        mov     [class_dw], eax
        shr     eax, 24                          ; class
        call    print_hex8
        mov     al, [class_dw+2]                 ; subclass
        call    print_hex8
        mov     si, msg_sp
        call    puts

        mov     al, 0x10                        ; the first region
        call    read_cfg
        call    print_hex32

        ; say plainly what the interesting ones are
        mov     al, [class_dw+3]
        cmp     al, 0x0C
        jne     .not_serial
        mov     al, [class_dw+2]
        cmp     al, 0x80
        jne     .other_serial
        mov     si, msg_i2c
        call    puts
        jmp     .named
.other_serial:
        cmp     al, 0x03
        jne     .named
        mov     si, msg_usb
        call    puts
        jmp     .named
.not_serial:
        cmp     al, 0x04                        ; multimedia
        jne     .named
        mov     si, msg_audio
        call    puts
.named:
        call    crlf
        call    page_break
        popad
        ret

; ---------------------------------------------------------------- config space
; AL = register (a multiple of 4), BX = bus:device:function -> EAX
read_cfg:
        push    dx
        push    ebx
        movzx   eax, al
        and     eax, 0xFC
        push    eax
        movzx   eax, bx                         ; bus in BH, device:function in BL
        mov     ecx, eax
        shr     ecx, 8                          ; bus
        and     eax, 0xFF                       ; device and function
        shl     ecx, 16
        shl     eax, 8
        or      eax, ecx
        pop     ecx
        or      eax, ecx
        or      eax, 0x80000000
        mov     dx, PCI_ADDR
        out     dx, eax
        mov     dx, PCI_DATA
        in      eax, dx
        pop     ebx
        pop     dx
        ret

; ---------------------------------------------------------------- screen
page_break:
        inc     word [lines]
        cmp     word [lines], 21
        jb      .done
        mov     word [lines], 0
        push    si
        mov     si, msg_more
        call    puts_screen
        pop     si
        xor     ah, ah
        int     0x16
        call    crlf
.done:  ret

puts:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:  pop     ax
        ret

; the same, but only to the screen: prompts do not belong in the file
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
        call    log_char
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

log_char:
        push    bx
        mov     bx, [log_len]
        cmp     bx, LOG_MAX - 1
        jae     .full
        mov     [log_buf+bx], al
        inc     word [log_len]
.full:  pop     bx
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

print_hex32:
        push    eax
        shr     eax, 16
        call    print_hex16
        pop     eax
        call    print_hex16
        ret

print_hex16:
        push    ax
        mov     al, ah
        call    print_hex8
        pop     ax
        call    print_hex8
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

msg_head:   db "PCI devices on this machine", 13, 10
            db "bus:dv.f vendor:device  class  first region", 13, 10
            db "-------------------------------------------------", 13, 10, 0
msg_sp:     db "  ", 0
msg_i2c:    db "  <- I2C controller", 0
msg_usb:    db "  <- USB controller", 0
msg_audio:  db "  <- audio", 0
msg_more:   db "-- press a key --", 13, 10, 0
msg_saved:  db "Written to C:\PCI.TXT", 13, 10, 0
msg_nosave: db "Could not write C:\PCI.TXT", 13, 10, 0
log_name:   db "\PCI.TXT", 0
msg_done:   db "-------------------------------------------------", 13, 10
            db "Class 0C80 is where a touchpad on an I2C bus would hang.", 13, 10, 0

LOG_MAX     equ 8192

section .bss
dev_id:     resd 1
class_dw:   resd 1
lines:      resw 1
log_len:    resw 1
log_buf:    resb LOG_MAX
