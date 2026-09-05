; EXETEST.EXE - a hand-built MZ executable that exercises the DOS services:
; relocation, PSP and environment, memory allocation, FindFirst/Next, file
; reading, IOCTL, and EXEC of another program.  Prints one line per check.
[BITS 16]
; ORG -512 so that in-memory (image-relative) offsets equal the label values:
; the 512-byte header loads at file 0, the image at file 512, and the loader
; places the image at load_seg:0.  With this ORG a label at file offset 512+X
; has value X, which is exactly its address in the loaded segment.
[ORG -512]

; ---- MZ header (one 512-byte page) ----------------------------------------------
header:
        db 'MZ'
        dw (image_end - image) % 512           ; bytes used in the last page
        dw (image_end - image + 511) / 512 + 1 ; pages, including the header page
        dw 2                                    ; relocation entries
        dw 32                                   ; header size in paragraphs
        dw 64                                   ; minalloc: 1 KB extra
        dw 0x0200                               ; maxalloc: 8 KB extra, leave room for EXEC
        dw (stack_seg - image) / 16             ; initial SS (relative)
        dw 2048                                 ; initial SP
        dw 0                                    ; checksum
        dw start - image                        ; initial IP
        dw 0                                    ; initial CS (relative)
        dw reloc_table - header                 ; relocation table offset
        dw 0                                    ; overlay number
reloc_table:
        dw fixup1 - image, 0
        dw fixup2 - image, 0
        times 512 - ($ - header) db 0

; ---- code and data (segment 0) --------------------------------------------------
image:
start:
        mov     ax, es
        mov     [cs:psp_seg], ax
        mov     ax, cs
        mov     ds, ax
        mov     es, ax

        mov     dx, msg_hello
        call    print

        ; --- relocation: the fixed-up words must equal our segment ---
        mov     dx, msg_reloc
        call    print
        mov     ax, [fixup1]
        mov     bx, cs
        cmp     ax, bx
        jne     .reloc_fail
        mov     ax, [fixup2]
        cmp     ax, bx
        jne     .reloc_fail
        call    pass
        jmp     .psp
.reloc_fail:
        call    fail
.psp:
        ; --- PSP: INT 20h at offset 0, environment with our path ---
        mov     dx, msg_psp
        call    print
        mov     es, [psp_seg]
        cmp     word [es:0], 0x20CD
        jne     .psp_fail
        mov     es, [es:0x2C]                   ; environment segment
        xor     di, di
.skip_env:
        cmp     word [es:di], 0
        je      .env_end
        inc     di
        jmp     .skip_env
.env_end:
        add     di, 4                           ; NUL NUL, count word
        push    ds
        push    es
        pop     ds
        mov     si, di
.env_path:
        lodsb
        or      al, al
        jz      .env_done
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     .env_path
.env_done:
        pop     ds
        mov     dx, msg_crlf
        call    print
        jmp     .tail
.psp_fail:
        call    fail
.tail:
        ; --- command tail ---
        mov     dx, msg_tail
        call    print
        mov     es, [psp_seg]
        movzx   cx, byte [es:0x80]
        mov     si, 0x81
.tail_ch:
        jcxz    .tail_done
        mov     dl, [es:si]
        mov     ah, 0x02
        int     0x21
        inc     si
        loop    .tail_ch
.tail_done:
        mov     dx, msg_crlf
        call    print

        ; --- memory: allocate, resize, free ---
        mov     dx, msg_mem
        call    print
        mov     ah, 0x48
        mov     bx, 0x100                       ; 4 KB
        int     0x21
        jc      .mem_fail
        mov     es, ax
        mov     ah, 0x4A
        mov     bx, 0x80                        ; shrink to 2 KB
        int     0x21
        jc      .mem_fail
        mov     ah, 0x49
        int     0x21
        jc      .mem_fail
        mov     ah, 0x48
        mov     bx, 0xFFFF                      ; impossible: must fail with BX = largest
        int     0x21
        jnc     .mem_fail
        cmp     bx, 0x1000                      ; at least 64 KB should be free
        jb      .mem_fail
        call    pass
        jmp     .version
.mem_fail:
        call    fail
.version:
        mov     dx, msg_ver
        call    print
        mov     ah, 0x30
        int     0x21
        cmp     al, 5
        jne     .ver_fail
        call    pass
        jmp     .cwd
.ver_fail:
        call    fail
.cwd:
        mov     dx, msg_cwd
        call    print
        mov     ah, 0x47
        xor     dl, dl
        mov     si, cwd_buf
        int     0x21
        mov     dl, '\'
        mov     ah, 0x02
        int     0x21
        mov     si, cwd_buf
        call    print_asciiz
        mov     dx, msg_crlf
        call    print

        ; --- FindFirst / FindNext on *.COM ---
        mov     dx, msg_find
        call    print
        mov     ah, 0x1A
        mov     dx, dta
        int     0x21
        mov     ah, 0x4E
        mov     cx, 0
        mov     dx, spec_com
        int     0x21
        jc      .find_done
.find_loop:
        mov     si, dta + 0x1E
        call    print_asciiz
        mov     dl, ' '
        mov     ah, 0x02
        int     0x21
        mov     ah, 0x4F
        int     0x21
        jnc     .find_loop
.find_done:
        mov     dx, msg_crlf
        call    print

        ; --- file: open README.TXT, read 20 bytes, size via seek ---
        mov     dx, msg_file
        call    print
        mov     ax, 0x3D00
        mov     dx, name_readme
        int     0x21
        jc      .file_fail
        mov     bx, ax
        mov     ah, 0x3F
        mov     cx, 20
        mov     dx, file_buf
        int     0x21
        jc      .file_fail
        mov     cx, ax
        mov     dx, file_buf
        mov     ah, 0x40
        push    bx
        mov     bx, 1
        int     0x21
        pop     bx
        mov     ax, 0x4202                      ; seek to end
        xor     cx, cx
        xor     dx, dx
        int     0x21
        jc      .file_fail
        push    ax
        mov     dx, msg_size
        call    print
        pop     ax
        call    print_dec
        mov     ah, 0x3E
        int     0x21
        mov     dx, msg_crlf
        call    print
        jmp     .ioctl
.file_fail:
        call    fail
.ioctl:
        mov     dx, msg_ioctl
        call    print
        mov     ax, 0x4400
        mov     bx, 1
        int     0x21
        jc      .ioctl_fail
        test    dl, 0x80
        jz      .ioctl_fail
        call    pass
        jmp     .exec
.ioctl_fail:
        call    fail
.exec:
        ; --- EXEC HELLO.COM with a command tail, then read its exit code ---
        mov     dx, msg_exec
        call    print
        mov     word [param_block+2], tail_hello
        mov     [param_block+4], cs
        mov     ax, cs
        mov     es, ax
        mov     bx, param_block
        mov     dx, name_hello
        mov     ax, 0x4B00
        int     0x21
        jc      .exec_fail
        mov     ah, 0x4D
        int     0x21
        push    ax
        mov     dx, msg_exec_back
        call    print
        pop     ax
        call    print_dec
        mov     dx, msg_crlf
        call    print
        jmp     .exit
.exec_fail:
        call    fail
.exit:
        mov     dx, msg_bye
        call    print
        mov     ax, 0x4C07                      ; exit code 7
        int     0x21

; ---- helpers ----------------------------------------------------------------
print:  mov     ah, 0x09
        int     0x21
        ret
pass:   mov     dx, msg_pass
        jmp     print
fail:   mov     dx, msg_fail
        jmp     print
print_asciiz:
        lodsb
        or      al, al
        jz      .done
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     print_asciiz
.done:  ret
print_dec:
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
.out:   pop     dx
        add     dl, '0'
        mov     ah, 0x02
        int     0x21
        loop    .out
        pop     dx
        pop     cx
        pop     bx
        ret

; ---- data -------------------------------------------------------------------
fixup1:         dw 0                            ; relocated: becomes our segment
fixup2:         dw 0
psp_seg:        dw 0
msg_hello:      db "EXETEST: MZ executable loaded", 13, 10, "$"
msg_reloc:      db "  relocations ......... $"
msg_psp:        db "  PSP / environment ... $"
msg_tail:       db "  command tail ........ $"
msg_mem:        db "  memory 48h/4Ah/49h .. $"
msg_ver:        db "  DOS version 5 ....... $"
msg_cwd:        db "  current directory ... $"
msg_find:       db "  FindFirst *.COM ..... $"
msg_file:       db "  file read ........... $"
msg_size:       db " size=$"
msg_ioctl:      db "  IOCTL stdout ........ $"
msg_exec:       db "  EXEC HELLO.COM ...... ", 13, 10, "$"
msg_exec_back:  db "  back from EXEC, exit code $"
msg_bye:        db "EXETEST done (exit code 7)", 13, 10, "$"
msg_pass:       db "PASS", 13, 10, "$"
msg_fail:       db "FAIL", 13, 10, "$"
msg_crlf:       db 13, 10, "$"
spec_com:       db "*.COM", 0
name_readme:    db "README.TXT", 0
name_hello:     db "HELLO.COM", 0
tail_hello:     db 13, " from EXETEST", 13
param_block:    dw 0, 0, 0, 0, 0, 0, 0
cwd_buf:        times 66 db 0
file_buf:       times 24 db 0
dta:            times 48 db 0
                align 16
stack_seg:      times 2048 db 0
image_end:
