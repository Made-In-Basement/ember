; HELLO.COM - the classic first program, using the DOS API (INT 21h)
[BITS 16]
[ORG 0x0100]

start:
        mov     ah, 0x09
        mov     dx, msg
        int     0x21
        ; print the command-line arguments (PSP:0080 = length, 0081.. = text)
        mov     cl, [0x80]
        or      cl, cl
        jz      .done
        mov     ah, 0x09
        mov     dx, msg_args
        int     0x21
        mov     si, 0x81
.next:  lodsb
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        dec     cl
        jnz     .next
        mov     ah, 0x09
        mov     dx, msg_crlf
        int     0x21
.done:
        mov     ax, 0x4C00
        int     0x21

msg:        db "Hello from a .COM program running on Ember!", 13, 10, "$"
msg_args:   db "You passed:$"
msg_crlf:   db 13, 10, "$"
