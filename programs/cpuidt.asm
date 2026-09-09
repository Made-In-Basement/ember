[BITS 16]
[ORG 0x100]
        mov     eax, 1
        cpuid
        mov     esi, edx
        mov     dx, m1
        mov     ah, 9
        int     0x21
        mov     eax, esi
        call    hex32
        mov     ax, 0x4C00
        int     0x21
hex32:  mov     cx, 8
.d:     rol     eax, 4
        push    eax
        and     al, 0x0F
        cmp     al, 10
        jb      .n
        add     al, 7
.n:     add     al, '0'
        mov     dl, al
        mov     ah, 2
        int     0x21
        pop     eax
        loop    .d
        ret
m1:     db "CPUID.1 EDX = $"
