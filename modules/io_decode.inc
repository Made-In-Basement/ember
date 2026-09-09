; =============================================================================
;  io_decode.inc - reading an IN or an OUT that the processor refused
; -----------------------------------------------------------------------------
;  Shared by the two hosts that impersonate a sound card: the DPMI host, whose
;  client runs at ring 3 in protected mode, and the monitor in sb.asm, whose
;  program runs in virtual-8086 mode.  Both arrive here the same way - a
;  general protection fault raised by the permission map in the task state
;  segment - and both have already worked out the linear address of the
;  instruction.  The only thing that differs is how wide a word is.
;
;  The includer must define, before including this file:
;     F_EAX, F_EDX, F_EIP    offsets into the frame EBP points at
;     IO_DEFAULT32           1 where a program's default operand size is 32
;                            bits (protected mode), 0 where it is 16 (V86)
;  and provide io_dev_in / io_dev_out.  GS must address memory linearly.
; =============================================================================

%ifndef IO_DEFAULT32
%define IO_DEFAULT32 1
%endif

[BITS 32]
; -----------------------------------------------------------------------------
; io_emulate: the instruction at GS:ESI is an IN or an OUT on a port the
;   permission map refuses.  Read it, let the card answer, and step over it.
;   CF=0 when it was handled.
;
;   The eight forms share one shape: opcodes E4-E7 and EC-EF, where bit 3
;   says the port is in DX rather than an immediate byte, bit 1 says it is an
;   OUT, and bit 0 says the operand is wide.  A 66h prefix swaps which width
;   "wide" means, so the same opcode is four bytes to a 32-bit program and
;   two to a program in virtual-8086 mode.
; -----------------------------------------------------------------------------
io_emulate:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        xor     ebx, ebx                        ; the instruction's length
        xor     ecx, ecx                        ; cl: an operand-size prefix
.prefix:
        mov     al, [gs:esi]
        cmp     al, 0x66
        je      .p66
        cmp     al, 0x67
        je      .pskip
        cmp     al, 0xF2
        je      .pskip
        cmp     al, 0xF3
        je      .pskip
        cmp     al, 0x26
        je      .pskip
        cmp     al, 0x2E
        je      .pskip
        cmp     al, 0x36
        je      .pskip
        cmp     al, 0x3E
        je      .pskip
        cmp     al, 0x64
        je      .pskip
        cmp     al, 0x65
        je      .pskip
        jmp     .opcode
.p66:   mov     cl, 1
.pskip: inc     esi
        inc     ebx
        cmp     ebx, 8
        jb      .prefix
        jmp     .unknown                        ; nothing sane is this long
.opcode:
        mov     ch, al
        and     al, 0xF4
        cmp     al, 0xE4
        jne     .unknown
        inc     ebx                             ; the opcode byte
        test    ch, 0x08
        jnz     .port_dx
        movzx   edx, byte [gs:esi + 1]          ; the port, written into the code
        inc     ebx
        jmp     .have_port
.port_dx:
        mov     edx, [ebp + F_EDX]
        and     edx, 0xFFFF
.have_port:
        test    ch, 0x01
        jz      .width1
%if IO_DEFAULT32
        cmp     cl, 1
        je      .width2                         ; 66h narrows it to a word
        mov     cl, 4
%else
        cmp     cl, 1
        jne     .width2                         ; 66h widens it to a dword
        mov     cl, 4
%endif
        jmp     .have_width
.width2:
        mov     cl, 2
        jmp     .have_width
.width1:
        mov     cl, 1
.have_width:
        test    ch, 0x02
        jnz     .an_out
        ; ---- IN: the card says something the program then believes ----
        call    io_dev_in
        cmp     cl, 1
        je      .in_byte
        cmp     cl, 2
        je      .in_word
        mov     [ebp + F_EAX], eax
        jmp     .stepped
.in_byte:
        mov     edx, [ebp + F_EAX]
        mov     dl, al
        mov     [ebp + F_EAX], edx
        jmp     .stepped
.in_word:
        mov     edx, [ebp + F_EAX]
        mov     dx, ax
        mov     [ebp + F_EAX], edx
        jmp     .stepped
        ; ---- OUT: whatever the program meant for the card ----
.an_out:
        mov     eax, [ebp + F_EAX]
        cmp     cl, 1
        jne     .out_wider
        and     eax, 0xFF
        jmp     .out_go
.out_wider:
        cmp     cl, 2
        jne     .out_go
        and     eax, 0xFFFF
.out_go:
        call    io_dev_out
.stepped:
        add     [ebp + F_EIP], ebx
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
.unknown:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
