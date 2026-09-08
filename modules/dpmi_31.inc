; =============================================================================
;  dpmi_31.inc - the INT 31h services (DPMI 0.9)
; -----------------------------------------------------------------------------
;  Entered from int_common with EBP -> the frame.  A function reads its
;  arguments from the frame and writes its answers there; int31_ok returns
;  with CF clear, int31_error with CF set and the code in AX.
; =============================================================================

[BITS 32]
int31_dispatch:
        and     dword [ebp + F_EFL], ~EFL_CF
        movzx   eax, word [ebp + F_EAX]
        mov     [cur_fn], ax
        mov     eax, [ebp + F_EBX]
        mov     [cur_bx], ax
        mov     eax, [ebp + F_ECX]
        mov     [cur_cx], ax
        mov     eax, [ebp + F_EDX]
        mov     [cur_dx], ax
        movzx   eax, word [ebp + F_EAX]
        inc     dword [call_count]
        mov     esi, int31_table
.scan:  cmp     word [esi], 0xFFFF
        je      .none
        cmp     [esi], ax
        je      .found
        add     esi, 6
        jmp     .scan
.found: jmp     dword [esi + 2]
.none:  mov     ax, DPMI_E_UNSUPPORTED
int31_error:
        ; remember what failed: function, error (a ring of 32, for the log)
        push    ecx
        mov     ecx, [gs:ERRLOG_LIN]
        and     ecx, 31
        mov     [gs:ERRLOG_LIN + 4 + ecx*4], ax
        push    ax
        mov     ax, [cur_fn]
        mov     [gs:ERRLOG_LIN + 6 + ecx*4], ax
        pop     ax
        inc     dword [gs:ERRLOG_LIN]
        pop     ecx
        mov     [ebp + F_EAX], ax
        or      dword [ebp + F_EFL], EFL_CF
        call    int31_trace
        jmp     int_exit
int31_ok:
        call    int31_trace
        jmp     int_exit

; int31_trace: the call and its answer into the ring at CALLTRACE_LIN:
;   function (bit 15 = failed), BX CX DX in, AX BX CX DX out
int31_trace:
        push    eax
        push    ecx
        push    esi
        mov     ax, [cur_fn]
        cmp     ah, 0x02                        ; vectors: too many to keep
        je      .skip
        mov     esi, [xarea]
        add     esi, CALLTRACE_OFF
        mov     ecx, [gs:esi]
        inc     dword [gs:esi]
        push    esi
        mov     eax, ecx
        push    edx
        xor     edx, edx
        mov     ecx, CALLTRACE_MAX
        div     ecx
        mov     esi, edx
        pop     edx
        shl     esi, 4
        add     esi, 4
        add     esi, [esp]                      ; the ring's base
        add     esp, 4
        mov     ax, [cur_fn]
        test    dword [ebp + F_EFL], EFL_CF
        jz      .fine
        or      ax, 0x8000
.fine:  mov     [gs:esi], ax
        mov     ax, [cur_bx]
        mov     [gs:esi + 2], ax
        mov     ax, [cur_cx]
        mov     [gs:esi + 4], ax
        push    ecx
        push    edx
        push    esi
        movzx   ecx, word [ebp + F_SS]
        mov     eax, [ebp + F_ESP]
        call    stack_addr                      ; the caller's return address
        mov     ax, [gs:eax]
        pop     esi
        pop     edx
        pop     ecx
        mov     [gs:esi + 6], ax
        mov     ax, [cur_fn]
        cmp     ax, 0x000B
        je      .desc
        cmp     ax, 0x000C
        je      .desc
        mov     eax, [ebp + F_EAX]
        mov     [gs:esi + 8], ax
        mov     eax, [ebp + F_EBX]
        mov     [gs:esi + 10], ax
        mov     eax, [ebp + F_ECX]
        mov     [gs:esi + 12], ax
        mov     eax, [ebp + F_EDX]
        mov     [gs:esi + 14], ax
        jmp     .skip
.desc:  mov     eax, [last_desc]
        mov     [gs:esi + 8], eax
        mov     eax, [last_desc + 4]
        mov     [gs:esi + 12], eax
        ; and 32 bytes of the caller's stack, for whoever is behind it
        push    ecx
        push    edx
        movzx   ecx, word [ebp + F_SS]
        mov     eax, [ebp + F_ESP]
        call    stack_addr
        mov     esi, eax
        mov     ecx, 8
.snap:  mov     eax, [gs:esi]
        mov     [gs:0x7EA0 + ecx*4 - 4], eax
        add     esi, 4
        loop    .snap
        pop     edx
        pop     ecx
.skip:  pop     esi
        pop     ecx
        pop     eax
        ret

section .data
int31_table:
        dw 0x0000
        dd f31_0000
        dw 0x0001
        dd f31_0001
        dw 0x0002
        dd f31_0002
        dw 0x0003
        dd f31_0003
        dw 0x0004
        dd int31_ok
        dw 0x0005
        dd int31_ok
        dw 0x0006
        dd f31_0006
        dw 0x0007
        dd f31_0007
        dw 0x0008
        dd f31_0008
        dw 0x0009
        dd f31_0009
        dw 0x000A
        dd f31_000A
        dw 0x000B
        dd f31_000B
        dw 0x000C
        dd f31_000C
        dw 0x000D
        dd f31_000D
        dw 0x0100
        dd f31_0100
        dw 0x0101
        dd f31_0101
        dw 0x0102
        dd f31_0102
        dw 0x0200
        dd f31_0200
        dw 0x0201
        dd f31_0201
        dw 0x0202
        dd f31_0202
        dw 0x0203
        dd f31_0203
        dw 0x0204
        dd f31_0204
        dw 0x0205
        dd f31_0205
        dw 0x0300
        dd f31_0300
        dw 0x0301
        dd f31_0301
        dw 0x0302
        dd f31_0302
        dw 0x0303
        dd f31_0303
        dw 0x0304
        dd f31_0304
        dw 0x0305
        dd f31_0305
        dw 0x0306
        dd f31_0306
        dw 0x0400
        dd f31_0400
        dw 0x0500
        dd f31_0500
        dw 0x0501
        dd f31_0501
        dw 0x0502
        dd f31_0502
        dw 0x0503
        dd f31_0503
        dw 0x0600
        dd int31_ok
        dw 0x0601
        dd int31_ok
        dw 0x0602
        dd int31_ok
        dw 0x0603
        dd int31_ok
        dw 0x0604
        dd f31_0604
        dw 0x0702
        dd int31_ok
        dw 0x0703
        dd int31_ok
        dw 0x0800
        dd f31_0800
        dw 0x0801
        dd int31_ok
        dw 0x0900
        dd f31_0900
        dw 0x0901
        dd f31_0901
        dw 0x0902
        dd f31_0902
        dw 0x0E00
        dd f31_0E00
        dw 0x0E01
        dd int31_ok
        dw 0xFFFF
section .text

; =============================================================================
; descriptors
; =============================================================================
; ---- 0000: allocate CX descriptors -> AX = the first selector ----
f31_0000:
        movzx   ecx, word [ebp + F_ECX]
        or      ecx, ecx
        jz      .bad
        call    ldt_alloc                       ; EBX = first index
        jc      .none
        mov     edx, ebx
.init:  mov     ecx, edx
        shl     ecx, 3
        or      ecx, 7
        call    desc_addr
        mov     dword [gs:esi], 0
        mov     dword [gs:esi + 4], 0x0000F200  ; data, writable, DPL 3, present
        inc     edx
        mov     ecx, edx
        sub     ecx, ebx
        cmp     cx, [ebp + F_ECX]
        jb      .init
        shl     ebx, 3
        or      ebx, 7
        mov     [ebp + F_EAX], bx
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_VALUE
        jmp     int31_error
.none:  mov     ax, DPMI_E_NODESC
        jmp     int31_error

; ---- 0001: free descriptor BX ----
f31_0001:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    ldt_free
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 0002: a selector for real-mode segment BX ----
f31_0002:
        movzx   eax, word [ebp + F_EBX]
        mov     esi, seg_cache
        mov     ecx, SEG_CACHE
.look:  cmp     word [esi + 2], 0
        je      .miss
        cmp     [esi], ax
        je      .hit
.next:  add     esi, 4
        loop    .look
        jmp     .full
.miss:  push    esi
        shl     eax, 4
        mov     ecx, 0xFFFF
        mov     dl, 0xF2
        call    desc_new
        pop     esi
        jc      .none
        mov     eax, [ebp + F_EBX]
        mov     [esi], ax
        mov     [esi + 2], bx
        mov     [ebp + F_EAX], bx
        jmp     int31_ok
.hit:   mov     ax, [esi + 2]
        mov     [ebp + F_EAX], ax
        jmp     int31_ok
.full:  ; no room to remember it: still hand one out
        movzx   eax, word [ebp + F_EBX]
        shl     eax, 4
        mov     ecx, 0xFFFF
        mov     dl, 0xF2
        call    desc_new
        jc      .none
        mov     [ebp + F_EAX], bx
        jmp     int31_ok
.none:  mov     ax, DPMI_E_NODESC
        jmp     int31_error

; ---- 0003: the selector increment ----
f31_0003:
        mov     word [ebp + F_EAX], SEL_INC
        jmp     int31_ok

; ---- 0006: base of BX -> CX:DX ----
f31_0006:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_base
        mov     [ebp + F_EDX], ax
        shr     eax, 16
        mov     [ebp + F_ECX], ax
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 0007: base of BX := CX:DX ----
f31_0007:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_addr
        mov     eax, [ebp + F_ECX]
        shl     eax, 16
        mov     ax, [ebp + F_EDX]
        mov     [gs:esi + 2], ax
        shr     eax, 16
        mov     [gs:esi + 4], al
        mov     [gs:esi + 7], ah
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 0008: limit of BX := CX:DX ----
f31_0008:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_addr
        mov     eax, [ebp + F_ECX]
        shl     eax, 16
        mov     ax, [ebp + F_EDX]
        mov     dl, 0                           ; granularity bit
        cmp     eax, 0xFFFFF
        jbe     .bytes
        mov     ecx, eax
        and     ecx, 0xFFF
        cmp     ecx, 0xFFF                      ; a page limit must end in FFF
        jne     .value
        shr     eax, 12
        mov     dl, 0x80
.bytes: mov     [gs:esi], ax
        shr     eax, 16
        and     al, 0x0F
        or      al, dl
        and     byte [gs:esi + 6], 0x70         ; keep D and AVL
        or      [gs:esi + 6], al
        jmp     int31_ok
.value: mov     ax, DPMI_E_VALUE
        jmp     int31_error
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 0009: access rights of BX := CX (CL = access, CH = the 386 bits) ----
f31_0009:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_addr
        mov     eax, [ebp + F_ECX]
        or      al, 0x60                        ; DPL 3, whatever was asked
        or      al, 0x10                        ; a code or data segment
        mov     [gs:esi + 5], al
        and     ah, 0xF0
        and     byte [gs:esi + 6], 0x0F
        or      [gs:esi + 6], ah
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 000A: a data alias of code selector BX -> AX ----
f31_000A:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_addr
        mov     eax, [gs:esi]
        mov     edx, [gs:esi + 4]
        push    eax
        push    edx
        mov     ecx, 1
        call    ldt_alloc
        pop     edx
        pop     eax
        jc      .none
        mov     ecx, ebx
        shl     ecx, 3
        or      ecx, 7
        push    ecx
        call    desc_addr
        mov     [gs:esi], eax
        and     edx, 0xFFFF00FF
        or      edx, 0x0000F200                 ; data, writable, DPL 3, present
        mov     [gs:esi + 4], edx
        pop     ecx
        mov     [ebp + F_EAX], cx
        jmp     int31_ok
.none:  mov     ax, DPMI_E_NODESC
        jmp     int31_error
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 000B: copy descriptor BX to ES:EDI ----
f31_000B:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_addr
        movzx   ecx, word [ebp + F_ES]
        call    desc_base
        add     eax, [ebp + F_EDI]
        mov     edx, [gs:esi]
        mov     [gs:eax], edx
        mov     [last_desc], edx
        mov     edx, [gs:esi + 4]
        mov     [gs:eax + 4], edx
        mov     [last_desc + 4], edx
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 000C: descriptor BX := the 8 bytes at ES:EDI ----
f31_000C:
        movzx   ecx, word [ebp + F_EBX]
        call    desc_valid
        jc      .bad
        call    desc_addr
        movzx   ecx, word [ebp + F_ES]
        call    desc_base
        add     eax, [ebp + F_EDI]
        mov     edx, [gs:eax]
        mov     [gs:esi], edx
        mov     [last_desc], edx
        mov     edx, [gs:eax + 4]
        mov     [last_desc + 4], edx
        or      edx, 0x00006000                 ; DPL 3
        mov     [gs:esi + 4], edx
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 000D: allocate the specific descriptor BX ----
f31_000D:
        movzx   eax, word [ebp + F_EBX]
        test    al, 4
        jz      .bad
        shr     eax, 3
        cmp     eax, LDT_ENTRIES
        jae     .bad
        bt      [ldt_used], eax
        jc      .bad
        bts     [ldt_used], eax
        mov     ecx, eax
        shl     ecx, 3
        or      ecx, 7
        call    desc_addr
        mov     dword [gs:esi], 0
        mov     dword [gs:esi + 4], 0x0000F200
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; =============================================================================
; DOS memory: the kernel's arena, reached through INT 21h in real mode
; =============================================================================
; rm_int21: rm_req holds the registers; run INT 21h in real mode
rm_int21:
        mov     byte [rm_vector], 0x21
        mov     byte [rm_kind], 0
        mov     word [rm_req + RC_SS], 0
        mov     word [rm_req + RC_SP], 0
        mov     word [rm_copy], 0
        mov     word [rm_req + RC_FLAGS], 0x0202
        call    depart
        ret

; ---- 0100: allocate BX paragraphs -> AX = segment, DX = first selector ----
f31_0100:
        mov     dword [rm_req + RC_EAX], 0x4800
        movzx   eax, word [ebp + F_EBX]
        mov     [rm_req + RC_EBX], eax
        call    rm_int21
        test    byte [rm_req + RC_FLAGS], EFL_CF
        jnz     .dos_error
        ; descriptors, one per 64 KB
        movzx   eax, word [ebp + F_EBX]
        shl     eax, 4                          ; bytes
        mov     edx, eax
        add     eax, 0xFFFF
        shr     eax, 16                         ; how many
        jnz     .have_count
        mov     eax, 1
.have_count:
        mov     ecx, eax
        push    edx
        call    ldt_alloc                       ; EBX = first index
        pop     edx
        jc      .no_desc
        ; remember the run, for freeing
        mov     esi, dosmem_table
        mov     ecx, DOSMEM_MAX
.slot:  cmp     word [esi + 2], 0
        je      .record
        add     esi, 4
        loop    .slot
        jmp     .fill                           ; unrecorded: free will not know
.record:
        mov     ecx, ebx
        shl     ecx, 3
        or      ecx, 7
        mov     [esi], cx
        mov     [esi + 2], ax
.fill:  ; EDX = bytes, EAX = count, EBX = first index
        mov     ecx, eax
        movzx   eax, word [rm_req + RC_EAX]     ; the segment
        mov     [ebp + F_EAX], ax
        shl     eax, 4
        mov     edi, eax                        ; EDI = base of the next piece
.piece: push    ecx
        mov     ecx, ebx
        shl     ecx, 3
        or      ecx, 7
        call    desc_addr
        pop     ecx
        mov     [gs:esi + 2], di
        mov     eax, edi
        shr     eax, 16
        mov     [gs:esi + 4], al
        mov     [gs:esi + 7], ah
        mov     eax, edx
        cmp     eax, 0x10000
        jbe     .last
        mov     eax, 0x10000
.last:  dec     eax
        mov     [gs:esi], ax
        mov     byte [gs:esi + 5], 0xF2
        mov     byte [gs:esi + 6], 0
        sub     edx, 0x10000
        add     edi, 0x10000
        inc     ebx
        loop    .piece
        ; DX = the first selector
        movzx   eax, word [ebp + F_EAX]
        mov     ecx, ebx
        sub     ecx, [esp-4]                    ; (not used)
        mov     esi, dosmem_table               ; find the first selector again:
        ; simpler: recompute from EBX - count
        movzx   ecx, word [ebp + F_EBX]
        shl     ecx, 4
        add     ecx, 0xFFFF
        shr     ecx, 16
        jnz     .cnt_ok
        mov     ecx, 1
.cnt_ok:
        sub     ebx, ecx
        shl     ebx, 3
        or      ebx, 7
        mov     [ebp + F_EDX], bx
        jmp     int31_ok
.dos_error:
        movzx   eax, word [rm_req + RC_EBX]
        mov     [ebp + F_EBX], ax               ; the largest available
        mov     ax, [rm_req + RC_EAX]
        jmp     int31_error
.no_desc:
        ; give the memory back
        movzx   eax, word [rm_req + RC_EAX]
        mov     [rm_req + RC_ES], ax
        mov     dword [rm_req + RC_EAX], 0x4900
        call    rm_int21
        mov     ax, DPMI_E_NODESC
        jmp     int31_error

; dosmem_find: DX (selector) -> ESI = its table entry, or CF
dosmem_find:
        movzx   eax, word [ebp + F_EDX]
        mov     esi, dosmem_table
        mov     ecx, DOSMEM_MAX
.look:  cmp     word [esi + 2], 0
        je      .next
        cmp     [esi], ax
        je      .found
.next:  add     esi, 4
        loop    .look
        stc
        ret
.found: clc
        ret

; ---- 0101: free the block whose first selector is DX ----
f31_0101:
        call    dosmem_find
        jc      .bad
        push    esi
        movzx   ecx, word [ebp + F_EDX]
        call    desc_base
        shr     eax, 4
        mov     [rm_req + RC_ES], ax
        mov     dword [rm_req + RC_EAX], 0x4900
        call    rm_int21
        pop     esi
        movzx   ecx, word [esi]
        movzx   edx, word [esi + 2]
        mov     word [esi + 2], 0
.free:  call    ldt_free
        add     ecx, 8
        dec     edx
        jnz     .free
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; ---- 0102: resize block DX to BX paragraphs ----
f31_0102:
        call    dosmem_find
        jc      .bad
        push    esi
        movzx   ecx, word [ebp + F_EDX]
        call    desc_base
        shr     eax, 4
        mov     [rm_req + RC_ES], ax
        mov     dword [rm_req + RC_EAX], 0x4A00
        movzx   eax, word [ebp + F_EBX]
        mov     [rm_req + RC_EBX], eax
        call    rm_int21
        pop     esi
        test    byte [rm_req + RC_FLAGS], EFL_CF
        jnz     .dos_error
        ; the descriptors: adjust the ones we have (growing past them fails)
        movzx   eax, word [ebp + F_EBX]
        shl     eax, 4
        mov     edx, eax                        ; bytes
        add     eax, 0xFFFF
        shr     eax, 16
        jnz     .cnt
        mov     eax, 1
.cnt:   cmp     ax, [esi + 2]
        ja      .no_desc
        movzx   ecx, word [esi]
        movzx   edi, word [esi + 2]             ; how many exist
.piece: push    ecx
        push    edi
        call    desc_addr
        mov     eax, edx
        or      edx, edx
        jle     .empty
        cmp     eax, 0x10000
        jbe     .last
        mov     eax, 0x10000
.last:  dec     eax
        mov     [gs:esi], ax
        jmp     .stored
.empty: mov     word [gs:esi], 0
.stored:
        pop     edi
        pop     ecx
        sub     edx, 0x10000
        add     ecx, 8
        dec     edi
        jnz     .piece
        jmp     int31_ok
.dos_error:
        movzx   eax, word [rm_req + RC_EBX]
        mov     [ebp + F_EBX], ax
        mov     ax, [rm_req + RC_EAX]
        jmp     int31_error
.no_desc:
        mov     ax, DPMI_E_NODESC
        jmp     int31_error
.bad:   mov     ax, DPMI_E_SELECTOR
        jmp     int31_error

; =============================================================================
; interrupt vectors
; =============================================================================
; ---- 0200: real-mode vector BL -> CX:DX ----
f31_0200:
        movzx   eax, byte [ebp + F_EBX]
        mov     eax, [gs:eax*4]
        mov     [ebp + F_EDX], ax
        shr     eax, 16
        mov     [ebp + F_ECX], ax
        jmp     int31_ok

; ---- 0201: real-mode vector BL := CX:DX ----
f31_0201:
        movzx   esi, byte [ebp + F_EBX]
        mov     eax, [ebp + F_ECX]
        shl     eax, 16
        mov     ax, [ebp + F_EDX]
        mov     [gs:esi*4], eax
        jmp     int31_ok

; ---- 0202: exception handler BL -> CX:EDX ----
f31_0202:
        movzx   eax, byte [ebp + F_EBX]
        cmp     eax, 32
        jae     .bad
        mov     edx, [exc_handlers + eax*8]
        movzx   ecx, word [exc_handlers + eax*8 + 4]
        or      edx, edx
        jnz     .have
        mov     edx, ret_stub_exc_default       ; a handler that ends the program
        mov     ecx, SEL_UCODE32
.have:  mov     [ebp + F_EDX], edx
        mov     [ebp + F_ECX], cx
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_VALUE
        jmp     int31_error

; ---- 0203: exception handler BL := CX:EDX ----
f31_0203:
        movzx   eax, byte [ebp + F_EBX]
        cmp     eax, 32
        jae     .bad
        mov     edx, [ebp + F_EDX]
        movzx   ecx, word [ebp + F_ECX]
        cmp     edx, ret_stub_exc_default
        jne     .set
        cmp     ecx, SEL_UCODE32
        jne     .set
        xor     edx, edx                        ; our default: no handler
        xor     ecx, ecx
.set:   mov     [exc_handlers + eax*8], edx
        mov     [exc_handlers + eax*8 + 4], cx
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_VALUE
        jmp     int31_error

; ---- 0204: protected-mode vector BL -> CX:EDX ----
f31_0204:
        movzx   eax, byte [ebp + F_EBX]
        mov     edx, [pm_vectors + eax*8]
        movzx   ecx, word [pm_vectors + eax*8 + 4]
        or      edx, edx
        jnz     .have
        ; the default: a stub that does what the host would
        mov     edx, eax
        imul    edx, DEF_STUB_SIZE
        add     edx, def_stubs
        mov     ecx, SEL_UCODE32
.have:  mov     [ebp + F_EDX], edx
        mov     [ebp + F_ECX], cx
        jmp     int31_ok

; ---- 0205: protected-mode vector BL := CX:EDX ----
f31_0205:
        movzx   eax, byte [ebp + F_EBX]
        mov     edx, [ebp + F_EDX]
        movzx   ecx, word [ebp + F_ECX]
        cmp     ecx, SEL_UCODE32
        jne     .set
        cmp     edx, def_stubs
        jb      .set
        cmp     edx, def_stubs + 256 * DEF_STUB_SIZE
        jae     .set
        xor     edx, edx                        ; back to the default
        xor     ecx, ecx
.set:   mov     [pm_vectors + eax*8], edx
        mov     [pm_vectors + eax*8 + 4], cx
        jmp     int31_ok

; =============================================================================
; the default protected-mode handlers a client may chain to
; =============================================================================
;   Each is "push n; int DEF_VECTOR".  A client that got the vector with
;   0204h calls it with an IRET frame; the host does what it would have
;   done without a hook, then returns as the IRET would.
%macro DEF_STUB 1
%%start:
        push    dword %1
        int     DEF_VECTOR
        times   DEF_STUB_SIZE - ($ - %%start) db 0x90
%endmacro
                align 4
def_stubs:
%assign v 0
%rep 256
        DEF_STUB v
%assign v v+1
%endrep

; a default exception handler that just reports the fault: RETF into it
ret_stub_exc_default:
        int     DEF_VECTOR                      ; (the frame says which)

; def_dispatch: INT DEF_VECTOR from a stub.  The vector number is on the
;   client's stack, above it the IRET frame of whoever called the stub.
def_dispatch:
        movzx   ecx, word [ebp + F_SS]
        mov     eax, [ebp + F_ESP]
        call    stack_addr                      ; EAX = linear top, EDX = mask
        mov     esi, eax
        mov     eax, [ebp + F_EIP]
        cmp     eax, ret_stub_exc_default + 2
        je      .exc_default
        push    dword [gs:esi]                  ; the vector number
        ; the caller's IRET frame becomes the return context
        mov     ecx, [gs:esi + 4]
        mov     [ebp + F_EIP], ecx
        mov     ecx, [gs:esi + 8]
        mov     [ebp + F_CS], ecx
        mov     ecx, [gs:esi + 12]
        and     ecx, ~(EFL_NT | EFL_IOPL)
        mov     [ebp + F_EFL], ecx
        mov     eax, [ebp + F_ESP]
        add     eax, 16                         ; the number and the frame, gone
        call    frame_set_esp
        pop     eax
        ; now do what the host does for that vector when it is not hooked
        cmp     eax, 0x08
        jb      .software
        cmp     eax, 0x0F
        jbe     .hardware
        cmp     eax, 0x70
        jb      .software
        cmp     eax, 0x77
        ja      .software
.hardware:
        mov     [rm_vector], al
        mov     byte [rm_kind], 0
        call    frame_to_req
        mov     word [rm_req + RC_SS], 0
        mov     word [rm_req + RC_SP], 0
        mov     word [rm_copy], 0
        call    depart
        jmp     int_exit
.software:
        cmp     eax, 32
        jae     reflect_sw
        ; an exception vector chained to: nothing sensible to do but return
        jmp     int_exit
.exc_default:
        ; a client's exception handler chained to ours: end the program
        ; with the frame's fault address
        add     esi, 0                          ; ESP -> error code, EIP, CS
        mov     eax, [gs:esi + 4]
        mov     [fault_eip], eax
        mov     eax, [gs:esi + 8]
        mov     [fault_cs], eax
        mov     eax, [gs:esi]
        mov     [fault_err], eax
        mov     byte [fault_vec], 0xFF
        mov     byte [rm_kind], 4
        call    depart

; =============================================================================
; real-mode calls
; =============================================================================
; rmcs_to_req: ES:EDI (of the caller) -> rm_req.  EDI = its linear address on
;   return.
rmcs_to_req:
        movzx   ecx, word [ebp + F_ES]
        call    desc_base
        add     eax, [ebp + F_EDI]
        mov     [rmcs_lin], eax
        mov     esi, eax
        mov     edi, rm_req
        push    ds
        pop     es
        push    ds
        push    gs
        pop     ds
        mov     ecx, RC_SIZE / 2
        rep     movsw
        pop     ds
        mov     ax, SEL_FLAT
        mov     es, ax
        ret

; req_to_rmcs: everything but SS:SP and CS:IP back into the caller's structure
req_to_rmcs:
        mov     esi, rm_req
        mov     edi, [rmcs_lin]
        mov     ecx, RC_IP / 2
        rep     movsw                           ; DS (host) -> ES (flat)
        ret

; rm_copy_setup: CX words of the caller's stack go along
rm_copy_setup:
        movzx   eax, word [ebp + F_ECX]
        mov     [rm_copy], ax
        movzx   ecx, word [ebp + F_SS]
        mov     eax, [ebp + F_ESP]
        call    stack_addr
        mov     [rm_copy_src], eax
        ret

; ---- 0300: simulate real-mode interrupt BL ----
f31_0300:
        call    rmcs_to_req
        call    rm_copy_setup
        mov     eax, [ebp + F_EBX]
        mov     [rm_vector], al
        mov     byte [rm_kind], 0
        ; ending the program this way is the usual way
        cmp     al, 0x21
        jne     .not_21
        mov     eax, [rm_req + RC_EAX]
        cmp     ah, 0x4C
        je      .terminate
        cmp     ah, 0x00
        je      .terminate
        cmp     ah, 0x31
        je      .terminate
        jmp     .go
.not_21:
        cmp     al, 0x20
        je      .terminate
        cmp     al, 0x27
        je      .terminate
.go:    call    depart
        call    req_to_rmcs
        jmp     int31_ok
.terminate:
        mov     byte [rm_kind], 3
        call    depart

; ---- 0301: call real-mode procedure with a far return frame ----
f31_0301:
        call    rmcs_to_req
        call    rm_copy_setup
        mov     byte [rm_kind], 1
        call    depart
        call    req_to_rmcs
        jmp     int31_ok

; ---- 0302: call real-mode procedure with an IRET frame ----
f31_0302:
        call    rmcs_to_req
        call    rm_copy_setup
        mov     byte [rm_kind], 2
        call    depart
        call    req_to_rmcs
        jmp     int31_ok

; ---- 0303: allocate a real-mode callback: DS:ESI = procedure, ES:EDI = structure ----
f31_0303:
        mov     esi, cb_used
        xor     ecx, ecx
.look:  cmp     byte [esi + ecx], 0
        je      .found
        inc     ecx
        cmp     ecx, CALLBACKS
        jb      .look
        mov     ax, DPMI_E_CALLBACK
        jmp     int31_error
.found: ; a selector for the real-mode stack it will be called on
        push    ecx
        xor     eax, eax
        mov     ecx, 0xFFFF
        mov     dl, 0xF2
        call    desc_new
        pop     ecx
        jc      .none
        mov     byte [cb_used + ecx], 1
        mov     edx, ecx
        imul    edx, CB_ENTRY
        mov     [cb_table + edx + CB_STACK_SEL], bx
        mov     eax, [ebp + F_ESI]
        mov     [cb_table + edx + CB_PROC], eax
        mov     eax, [ebp + F_DS]
        mov     [cb_table + edx + CB_PROC_SEL], ax
        mov     eax, [ebp + F_EDI]
        mov     [cb_table + edx + CB_RMCS], eax
        mov     eax, [ebp + F_ES]
        mov     [cb_table + edx + CB_RMCS_SEL], ax
        ; CX:DX = the real-mode address of its stub
        mov     eax, [host_base]
        shr     eax, 4
        mov     [ebp + F_ECX], ax
        mov     eax, ecx
        imul    eax, CB_STUB_SIZE
        add     eax, cb_stubs
        mov     [ebp + F_EDX], ax
        jmp     int31_ok
.none:  mov     ax, DPMI_E_NODESC
        jmp     int31_error

; ---- 0304: free callback CX:DX ----
f31_0304:
        movzx   eax, word [ebp + F_EDX]
        sub     eax, cb_stubs
        jb      .bad
        xor     edx, edx
        mov     ecx, CB_STUB_SIZE
        div     ecx
        cmp     eax, CALLBACKS
        jae     .bad
        cmp     byte [cb_used + eax], 0
        je      .bad
        mov     byte [cb_used + eax], 0
        imul    eax, CB_ENTRY
        movzx   ecx, word [cb_table + eax + CB_STACK_SEL]
        call    ldt_free
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_CALLBACK
        jmp     int31_error

; ---- 0305: state save/restore addresses: nothing to save ----
f31_0305:
        mov     word [ebp + F_EAX], 0
        mov     eax, [host_base]
        shr     eax, 4
        mov     [ebp + F_EBX], ax
        mov     word [ebp + F_ECX], rm_retf_stub
        mov     word [ebp + F_ESI], SEL_UCODE32
        mov     dword [ebp + F_EDI], pm_retf_stub
        jmp     int31_ok

; ---- 0306: raw mode switch addresses ----
;   BX:CX = where real mode jumps to become protected, SI:EDI the other way
f31_0306:
        mov     eax, [host_base]
        shr     eax, 4
        mov     [ebp + F_EBX], ax
        mov     word [ebp + F_ECX], raw_rm_entry
        mov     word [ebp + F_ESI], SEL_UCODE32
        mov     dword [ebp + F_EDI], raw_pm_stub
        jmp     int31_ok

pm_retf_stub:   retf
[BITS 16]
rm_retf_stub:   retf
[BITS 32]

; ---- 0400: version ----
f31_0400:
        mov     word [ebp + F_EAX], 0x005A      ; 0.90
        mov     word [ebp + F_EBX], 0x0001      ; 32-bit host, real mode underneath
        mov     word [ebp + F_ECX], 0x0004      ; a 486 or better
        mov     word [ebp + F_EDX], 0x0870      ; the controllers at 08h and 70h
        jmp     int31_ok

; =============================================================================
; memory
; =============================================================================
; ---- 0500: free memory information into the 48 bytes at ES:EDI ----
f31_0500:
        movzx   ecx, word [ebp + F_ES]
        call    desc_base
        add     eax, [ebp + F_EDI]
        mov     edi, eax
        call    mem_largest                     ; EAX
        mov     [gs:edi], eax
        shr     eax, 12
        mov     [gs:edi + 4], eax               ; max unlocked pages
        mov     [gs:edi + 8], eax               ; max locked pages
        mov     eax, [xarea_kb]
        shl     eax, 10
        sub     eax, XA_POOL
        shr     eax, 12
        mov     [gs:edi + 12], eax              ; linear pages
        mov     [gs:edi + 16], eax              ; unlocked pages
        mov     [gs:edi + 24], eax              ; physical pages
        call    mem_free_total
        shr     eax, 12
        mov     [gs:edi + 20], eax              ; free pages
        mov     [gs:edi + 28], eax              ; free linear pages
        mov     dword [gs:edi + 32], 0          ; no paging file
        mov     dword [gs:edi + 36], 0xFFFFFFFF
        mov     dword [gs:edi + 40], 0xFFFFFFFF
        mov     dword [gs:edi + 44], 0xFFFFFFFF
        jmp     int31_ok

; ---- 0501: allocate BX:CX bytes -> BX:CX = address, SI:DI = handle ----
f31_0501:
        mov     eax, [ebp + F_EBX]
        shl     eax, 16
        mov     ax, [ebp + F_ECX]
        mov     ecx, eax
        call    mem_alloc                       ; EAX = base, EDX = handle
        jc      .none
        mov     [ebp + F_ECX], ax
        shr     eax, 16
        mov     [ebp + F_EBX], ax
        mov     [ebp + F_EDI], dx
        shr     edx, 16
        mov     [ebp + F_ESI], dx
        jmp     int31_ok
.none:  mov     ax, DPMI_E_NOPHYS
        jmp     int31_error

; ---- 0502: free handle SI:DI ----
f31_0502:
        mov     edx, [ebp + F_ESI]
        shl     edx, 16
        mov     dx, [ebp + F_EDI]
        call    mem_free
        jc      .bad
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_HANDLE
        jmp     int31_error

; ---- 0503: resize handle SI:DI to BX:CX bytes ----
f31_0503:
        mov     edx, [ebp + F_ESI]
        shl     edx, 16
        mov     dx, [ebp + F_EDI]
        mov     eax, [ebp + F_EBX]
        shl     eax, 16
        mov     ax, [ebp + F_ECX]
        mov     ecx, eax
        call    mem_resize                      ; EAX = base, EDX = handle
        jc      .bad
        mov     [ebp + F_ECX], ax
        shr     eax, 16
        mov     [ebp + F_EBX], ax
        mov     [ebp + F_EDI], dx
        shr     edx, 16
        mov     [ebp + F_ESI], dx
        jmp     int31_ok
.bad:   mov     ax, DPMI_E_NOPHYS
        jmp     int31_error

; ---- 0604: page size ----
f31_0604:
        mov     word [ebp + F_EBX], 0
        mov     word [ebp + F_ECX], 4096
        jmp     int31_ok

; ---- 0800: physical address mapping: linear is physical here ----
f31_0800:
        jmp     int31_ok

; ---- 0900-0902: the virtual interrupt flag is the real one ----
f31_0900:
        call    vif_get
        and     dword [ebp + F_EFL], ~EFL_IF
        jmp     int31_ok
f31_0901:
        call    vif_get
        or      dword [ebp + F_EFL], EFL_IF
        jmp     int31_ok
f31_0902:
        call    vif_get
        jmp     int31_ok
vif_get:
        mov     al, 0
        test    dword [ebp + F_EFL], EFL_IF
        jz      .off
        mov     al, 1
.off:   mov     [ebp + F_EAX], al
        ret

; ---- 0E00: coprocessor status ----
f31_0E00:
        mov     word [ebp + F_EAX], 0x0045      ; a 486 coprocessor, enabled
        jmp     int31_ok

; =============================================================================
; the memory pool: first fit over a table of blocks, coalesced on free
; =============================================================================
; mem_alloc: ECX bytes -> EAX = base, EDX = handle.  CF=1 if none.
mem_alloc:
        push    esi
        push    edi
        push    ebx
        add     ecx, 15
        and     ecx, ~15
        jnz     .sized
        mov     ecx, 16
.sized: mov     esi, mem_blocks
        mov     ebx, MEM_BLOCKS
.scan:  cmp     byte [esi + MB_STATE], 1
        jne     .next
        cmp     [esi + MB_SIZE_], ecx
        jae     .found
.next:  add     esi, MB_SIZE
        dec     ebx
        jnz     .scan
        jmp     .none
.found: mov     eax, [esi + MB_SIZE_]
        sub     eax, ecx
        jz      .whole
        ; split: a spare entry takes the remainder
        mov     edi, mem_blocks
        mov     ebx, MEM_BLOCKS
.spare: cmp     byte [edi + MB_STATE], 0
        je      .have_spare
        add     edi, MB_SIZE
        dec     ebx
        jnz     .spare
        jmp     .whole                          ; no spare: give the whole block
.have_spare:
        mov     [edi + MB_SIZE_], eax
        mov     eax, [esi + MB_BASE]
        add     eax, ecx
        mov     [edi + MB_BASE], eax
        mov     byte [edi + MB_STATE], 1
        mov     [esi + MB_SIZE_], ecx
.whole: mov     byte [esi + MB_STATE], 2
        mov     eax, [esi + MB_BASE]
        mov     edx, esi
        sub     edx, mem_blocks
        push    eax
        mov     eax, edx
        xor     edx, edx
        mov     ecx, MB_SIZE
        div     ecx
        inc     eax
        mov     edx, eax
        pop     eax
        pop     ebx
        pop     edi
        pop     esi
        clc
        ret
.none:  pop     ebx
        pop     edi
        pop     esi
        stc
        ret

; mem_find: EDX = handle -> ESI = its entry.  CF=1 if it is not one in use.
mem_find:
        push    eax
        or      edx, edx
        jz      .bad
        cmp     edx, MEM_BLOCKS
        ja      .bad
        mov     eax, edx
        dec     eax
        imul    eax, MB_SIZE
        add     eax, mem_blocks
        mov     esi, eax
        cmp     byte [esi + MB_STATE], 2
        jne     .bad
        pop     eax
        clc
        ret
.bad:   pop     eax
        stc
        ret

; mem_free: EDX = handle.  CF=1 if it was not one.
mem_free:
        push    esi
        call    mem_find
        jc      .out
        mov     byte [esi + MB_STATE], 1
        call    mem_coalesce
        clc
.out:   pop     esi
        ret

; mem_coalesce: join free blocks that touch
mem_coalesce:
        pushad
        mov     esi, mem_blocks
        mov     ecx, MEM_BLOCKS
.outer: cmp     byte [esi + MB_STATE], 1
        jne     .next_outer
        mov     edi, mem_blocks
        mov     edx, MEM_BLOCKS
.inner: cmp     byte [edi + MB_STATE], 1
        jne     .next_inner
        cmp     edi, esi
        je      .next_inner
        mov     eax, [esi + MB_BASE]
        add     eax, [esi + MB_SIZE_]
        cmp     eax, [edi + MB_BASE]
        jne     .next_inner
        mov     eax, [edi + MB_SIZE_]
        add     [esi + MB_SIZE_], eax
        mov     byte [edi + MB_STATE], 0
        jmp     .inner                          ; the same block may touch another
.next_inner:
        add     edi, MB_SIZE
        dec     edx
        jnz     .inner
.next_outer:
        add     esi, MB_SIZE
        loop    .outer
        popad
        ret

; mem_resize: EDX = handle, ECX = new size -> EAX = base, EDX = handle.
;   In place when the block can shrink or the space after it is free;
;   otherwise a new block, the contents moved, the old one freed.
mem_resize:
        push    esi
        push    edi
        push    ebx
        add     ecx, 15
        and     ecx, ~15
        jnz     .sized
        mov     ecx, 16
.sized: call    mem_find
        jc      .bad
        mov     eax, [esi + MB_SIZE_]
        cmp     ecx, eax
        je      .same
        jb      .shrink
        ; grow: is the block after it free and big enough?
        mov     eax, [esi + MB_BASE]
        add     eax, [esi + MB_SIZE_]
        mov     edi, mem_blocks
        mov     ebx, MEM_BLOCKS
.after: cmp     byte [edi + MB_STATE], 1
        jne     .next_after
        cmp     [edi + MB_BASE], eax
        je      .found_after
.next_after:
        add     edi, MB_SIZE
        dec     ebx
        jnz     .after
        jmp     .move
.found_after:
        mov     eax, ecx
        sub     eax, [esi + MB_SIZE_]           ; extra wanted
        cmp     [edi + MB_SIZE_], eax
        jb      .move
        sub     [edi + MB_SIZE_], eax
        add     [edi + MB_BASE], eax
        mov     [esi + MB_SIZE_], ecx
        cmp     dword [edi + MB_SIZE_], 0
        jne     .same
        mov     byte [edi + MB_STATE], 0
        jmp     .same
.shrink:
        ; the tail becomes a free block, if an entry is spare
        mov     edi, mem_blocks
        mov     ebx, MEM_BLOCKS
.spare: cmp     byte [edi + MB_STATE], 0
        je      .have_spare
        add     edi, MB_SIZE
        dec     ebx
        jnz     .spare
        jmp     .same                           ; keep it all: harmless
.have_spare:
        mov     eax, [esi + MB_SIZE_]
        sub     eax, ecx
        mov     [edi + MB_SIZE_], eax
        mov     eax, [esi + MB_BASE]
        add     eax, ecx
        mov     [edi + MB_BASE], eax
        mov     byte [edi + MB_STATE], 1
        mov     [esi + MB_SIZE_], ecx
        call    mem_coalesce
.same:  mov     eax, [esi + MB_BASE]
        pop     ebx
        pop     edi
        pop     esi
        clc
        ret
.move:  ; a new block, the old contents copied across
        push    edx
        push    esi
        call    mem_alloc                       ; EAX = base, EDX = handle
        pop     esi
        jc      .move_failed
        pop     ebx                             ; the old handle
        push    edx
        push    eax
        mov     edi, eax
        push    esi
        mov     ecx, [esi + MB_SIZE_]
        shr     ecx, 2
        mov     esi, [esi + MB_BASE]
        push    ds
        push    gs
        pop     ds
        rep     movsd                           ; flat to flat
        pop     ds
        pop     esi
        mov     edx, ebx
        call    mem_free
        pop     eax
        pop     edx
        pop     ebx
        pop     edi
        pop     esi
        clc
        ret
.move_failed:
        pop     edx
.bad:   pop     ebx
        pop     edi
        pop     esi
        stc
        ret

; mem_largest: EAX = the largest free block
mem_largest:
        push    esi
        push    ecx
        xor     eax, eax
        mov     esi, mem_blocks
        mov     ecx, MEM_BLOCKS
.scan:  cmp     byte [esi + MB_STATE], 1
        jne     .next
        cmp     [esi + MB_SIZE_], eax
        jbe     .next
        mov     eax, [esi + MB_SIZE_]
.next:  add     esi, MB_SIZE
        loop    .scan
        pop     ecx
        pop     esi
        ret

; mem_free_total: EAX = free bytes
mem_free_total:
        push    esi
        push    ecx
        xor     eax, eax
        mov     esi, mem_blocks
        mov     ecx, MEM_BLOCKS
.scan:  cmp     byte [esi + MB_STATE], 1
        jne     .next
        add     eax, [esi + MB_SIZE_]
.next:  add     esi, MB_SIZE
        loop    .scan
        pop     ecx
        pop     esi
        ret

section .data
                align 4
rmcs_lin:       dd 0
last_desc:      dd 0, 0
cur_fn:         dw 0
cur_bx:         dw 0
cur_cx:         dw 0
cur_dx:         dw 0
call_count:     dd 0
section .text
