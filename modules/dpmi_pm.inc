; =============================================================================
;  dpmi_pm.inc - the 32-bit side of the DPMI host
; -----------------------------------------------------------------------------
;  Ring-0 conventions here: DS = SEL_HDATA32, so this file's labels address
;  the host's own data; GS = SEL_FLAT for anything by linear address; SS =
;  SEL_FLAT with ESP in the ring-0 stack, which is in the client's XMS block.
;  Interrupts stay off in ring 0: every frame that arrives here came from
;  ring 3 (or the host was mid-fault, which is reported as such).
;
;  The frame an interrupt leaves, once int_common has pushed everything:
;     +0 GS  +4 FS  +8 ES  +12 DS  +16 EDI  +20 ESI  +24 EBP  +28 (ESP)
;     +32 EBX  +36 EDX  +40 ECX  +44 EAX  +48 vector  +52 error code
;     +56 EIP  +60 CS  +64 EFLAGS  +68 ESP  +72 SS
;  Bit 8 of the vector marks a hardware interrupt the controller confirmed.
; =============================================================================

F_GS            equ 0
F_FS            equ 4
F_ES            equ 8
F_DS            equ 12
F_EDI           equ 16
F_ESI           equ 20
F_EBP           equ 24
F_EBX           equ 32
F_EDX           equ 36
F_ECX           equ 40
F_EAX           equ 44
F_VEC           equ 48
F_ERR           equ 52
F_EIP           equ 56
F_CS            equ 60
F_EFL           equ 64
F_ESP           equ 68
F_SS            equ 72

VEC_IRQ         equ 0x100                       ; set in F_VEC for a real IRQ

EFL_CF          equ 0x0001
EFL_TF          equ 0x0100
EFL_IF          equ 0x0200
EFL_NT          equ 0x4000
EFL_IOPL        equ 0x3000

; error codes
DPMI_E_UNSUPPORTED equ 0x8001
DPMI_E_NODESC   equ 0x8011
DPMI_E_NOLINEAR equ 0x8012
DPMI_E_NOPHYS   equ 0x8013
DPMI_E_VALUE    equ 0x8021
DPMI_E_SELECTOR equ 0x8022
DPMI_E_HANDLE   equ 0x8023
DPMI_E_CALLBACK equ 0x8024

; FLAGS_KNOWN: EFLAGS as the host wants them - NT clear above all, and DF,
;   TF and IOPL with it.  Real mode's flags cross into protected mode on the
;   far jump, and NT turns every IRET here into a task return.  It needs a
;   stack, so it goes after ESP is loaded, never before.
%macro FLAGS_KNOWN 0
        push    dword 2
        popfd
%endmacro

[BITS 32]
; =============================================================================
; pm_first_entry: protected mode for the first time with this client.
;   Interrupts are off.  The client's registers are in ent_*.
; =============================================================================
pm_first_entry:
        mov     ax, SEL_HDATA32
        mov     ds, ax
        mov     ax, SEL_FLAT
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, [r0_top]
        ; Real mode's flags came through the far jump untouched.  NT there is
        ; only a bit; here it turns the IRET at the end of this routine into a
        ; task return through the TSS's back link, which is null: #TS(0) before
        ; the client's first instruction.  A known EFLAGS instead - and not
        ; before now: this is the first instant there is a stack to push on.
        FLAGS_KNOWN
        mov     ax, SEL_TSS
        ltr     ax
        mov     ax, SEL_LDT
        lldt    ax
        mov     eax, [xarea]
        add     eax, XA_LDT
        mov     [ldt_lin], eax
        mov     eax, [xarea]
        mov     dword [gs:eax + CALLTRACE_OFF], 0
        ; the numeric coprocessor, for clients that use it
        mov     eax, cr0
        and     eax, ~0x0C                      ; EM=0, TS=0
        or      eax, 0x22                       ; MP=1, NE=1
        mov     cr0, eax
        fninit
        ; ---- the client's real-mode segments as descriptors ----
        movzx   eax, word [ent_cs]
        shl     eax, 4
        mov     ecx, 0xFFFF
        mov     dl, 0xFA                        ; code, DPL 3
        call    desc_new                        ; -> BX = selector
        mov     [cl_cs], bx
        movzx   eax, word [ent_ds]
        shl     eax, 4
        mov     ecx, 0xFFFF
        mov     dl, 0xF2                        ; data, DPL 3
        call    desc_new
        mov     [cl_ds], bx
        movzx   eax, word [ent_ss]
        shl     eax, 4
        mov     ecx, 0xFFFF
        mov     dl, 0xF2
        call    desc_new
        mov     [cl_ss], bx
        movzx   eax, word [client_psp]
        shl     eax, 4
        mov     ecx, 0x00FF
        mov     dl, 0xF2
        call    desc_new
        mov     [cl_psp], bx
        ; the PSP's environment field becomes a selector while the client
        ; runs, as the specification asks; the paragraph comes back at the end
        movzx   edi, word [client_psp]
        shl     edi, 4
        movzx   eax, word [gs:edi + 0x2C]
        mov     [psp_env_seg], ax
        shl     eax, 4
        mov     ecx, 0xFFFF
        mov     dl, 0xF2
        call    desc_new                        ; (ESI is the descriptor after this)
        jc      .no_env
        mov     [gs:edi + 0x2C], bx
.no_env:
        ; a data selector on the locked stack, for the client's handlers
        mov     eax, [xarea]
        add     eax, XA_LOCKED
        mov     ecx, XA_LOCKED_TOP - XA_LOCKED - 1
        mov     dl, 0xF2
        call    desc_new
        mov     [locked_sel], bx
        call    desc_set_32                     ; a 32-bit stack
        ; ---- into the client: a far return, in effect ----
        movzx   eax, word [ent_flags]
        or      eax, EFL_IF
        and     eax, ~(EFL_TF | EFL_NT | EFL_IOPL)
        push    dword [cl_ss]                   ; SS
        movzx   ecx, word [ent_sp]
        push    ecx                             ; ESP
        push    eax                             ; EFLAGS
        push    dword [cl_cs]                   ; CS
        movzx   ecx, word [ent_ip]
        push    ecx                             ; EIP
        mov     ax, [cl_psp]
        mov     es, ax
        xor     ax, ax
        mov     fs, ax
        mov     gs, ax
        mov     eax, [ent_eax]
        mov     ebx, [ent_ebx]
        mov     ecx, [ent_ecx]
        mov     edx, [ent_edx]
        mov     esi, [ent_esi]
        mov     edi, [ent_edi]
        mov     ebp, [ent_ebp]
        mov     ds, [cl_ds]
        iretd

; =============================================================================
; the 256 stubs the IDT points at
; =============================================================================
;   Vectors that carry an error code push only their number; the rest push a
;   zero first so the frame is the same shape for everyone.
%macro VEC_STUB 1
%%start:
%if %1 == 8 || %1 == 10 || %1 == 11 || %1 == 12 || %1 == 13 || %1 == 14 || %1 == 17
        push    dword %1
        jmp     int_common
%else
        push    dword 0
        push    dword %1
        jmp     int_common
%endif
        times   VEC_STUB_SIZE - ($ - %%start) db 0x90
%endmacro

                align 4
vec_stubs:
%assign v 0
%rep 256
        VEC_STUB v
%assign v v+1
%endrep

; =============================================================================
; int_common: sort out the frame, then decide what the interrupt was
; =============================================================================
int_common:
        ; vectors 8-15: the controller says whether this is its request
        cmp     dword [esp], 8
        jb      .sorted
        cmp     dword [esp], 15
        ja      .sorted
        push    eax
        push    ecx
        mov     al, 0x0B                        ; OCW3: read the in-service register
        out     0x20, al
        jmp     short $+2                       ; the controller needs a moment
        jmp     short $+2                       ;  between the write and the read
        in      al, 0x20
        mov     ecx, [esp+8]
        sub     ecx, 8
        bt      eax, ecx
        jnc     .not_irq
        pop     ecx
        pop     eax
        ; a request: those with an error code pushed only their number
        cmp     dword [esp], 9
        je      .mark_irq
        cmp     dword [esp], 15
        je      .mark_irq
        xchg    eax, [esp]                      ; EAX = the number, EAX saved in its place
        push    eax                             ; [number] [saved EAX] [EIP] ...
        mov     eax, [esp + 4]                  ; EAX back
        mov     dword [esp + 4], 0              ; the error code that is not there
.mark_irq:
        or      dword [esp], VEC_IRQ
        jmp     .sorted
.not_irq:
        pop     ecx
        pop     eax
        cmp     dword [esp], 15                 ; vector 15 and not in service:
        jne     .sorted                         ;  a spurious IRQ7, nothing more
        add     esp, 8
        iretd
.sorted:
        pushad
        push    ds
        push    es
        push    fs
        push    gs
        mov     ax, SEL_HDATA32
        mov     ds, ax
        mov     ax, SEL_FLAT
        mov     es, ax
        mov     gs, ax
        mov     ebp, esp
        cld
        ; a fault inside the host itself: say so and stop
        test    byte [ebp + F_CS], 3
        jz      host_fault
        mov     eax, [ebp + F_VEC]
        test    eax, VEC_IRQ
        jnz     hw_interrupt
        cmp     eax, 0x70
        jb      .not_slave
        cmp     eax, 0x77
        ja      .not_slave
        or      dword [ebp + F_VEC], VEC_IRQ
        jmp     hw_interrupt
.not_slave:
        cmp     eax, RET_VECTOR
        je      ret_from_client
        cmp     eax, DEF_VECTOR
        je      def_dispatch
        cmp     eax, RAW_VECTOR
        je      raw_dispatch
        cmp     eax, 0x31
        je      int31_dispatch
        cmp     eax, 32
        jb      exception
        ; ---- a software interrupt ----
        jmp     sw_interrupt

; int_exit: back to the client with the frame as it now is
int_exit:
        mov     esp, ebp
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popad
        add     esp, 8
        iretd

; =============================================================================
; hw_interrupt: the client's handler if it has one, else real mode's
; =============================================================================
hw_interrupt:
        movzx   eax, byte [ebp + F_VEC]
        cmp     dword [pm_vectors + eax*8], 0
        jne     deliver_int
        ; reflect: registers do not matter to a hardware handler
        mov     [rm_vector], al
        mov     byte [rm_kind], 0
        call    frame_to_req
        mov     word [rm_req + RC_SS], 0
        mov     word [rm_req + RC_SP], 0
        mov     word [rm_copy], 0
        call    depart
        jmp     int_exit

; =============================================================================
; sw_interrupt: INT n from the client
; =============================================================================
sw_interrupt:
        movzx   eax, byte [ebp + F_VEC]
        cmp     dword [pm_vectors + eax*8], 0
        jne     deliver_int
reflect_sw:
        ; ---- ending the program?  the real-mode call never returns ----
        cmp     al, 0x20
        je      .terminate
        cmp     al, 0x27
        je      .terminate
        cmp     al, 0x21
        jne     .go
        mov     ecx, [ebp + F_EAX]
        cmp     ch, 0x4C
        je      .terminate
        cmp     ch, 0x00
        je      .terminate
        cmp     ch, 0x31
        je      .terminate
.go:    mov     [rm_vector], al
        mov     byte [rm_kind], 0
        call    frame_to_req
        mov     word [rm_req + RC_SS], 0
        mov     word [rm_req + RC_SP], 0
        mov     word [rm_copy], 0
        call    depart
        call    req_to_frame
        jmp     int_exit
.terminate:
        mov     [rm_vector], al
        call    frame_to_req
        mov     byte [rm_kind], 3
        call    depart                          ; does not come back

; frame_to_req: the client's general registers into the real-mode request
frame_to_req:
        mov     eax, [ebp + F_EAX]
        mov     [rm_req + RC_EAX], eax
        mov     eax, [ebp + F_EBX]
        mov     [rm_req + RC_EBX], eax
        mov     eax, [ebp + F_ECX]
        mov     [rm_req + RC_ECX], eax
        mov     eax, [ebp + F_EDX]
        mov     [rm_req + RC_EDX], eax
        mov     eax, [ebp + F_ESI]
        mov     [rm_req + RC_ESI], eax
        mov     eax, [ebp + F_EDI]
        mov     [rm_req + RC_EDI], eax
        mov     eax, [ebp + F_EBP]
        mov     [rm_req + RC_EBP], eax
        mov     eax, [ebp + F_EFL]
        mov     [rm_req + RC_FLAGS], ax
        ; segment registers: real-mode code gets the selector values, which
        ; is what every host does; a call that needs real addresses uses 0300h
        mov     eax, [ebp + F_DS]
        mov     [rm_req + RC_DS], ax
        mov     eax, [ebp + F_ES]
        mov     [rm_req + RC_ES], ax
        mov     eax, [ebp + F_FS]
        mov     [rm_req + RC_FS], ax
        mov     eax, [ebp + F_GS]
        mov     [rm_req + RC_GS], ax
        ret

; req_to_frame: the general registers and the arithmetic flags back
req_to_frame:
        mov     eax, [rm_req + RC_EAX]
        mov     [ebp + F_EAX], eax
        mov     eax, [rm_req + RC_EBX]
        mov     [ebp + F_EBX], eax
        mov     eax, [rm_req + RC_ECX]
        mov     [ebp + F_ECX], eax
        mov     eax, [rm_req + RC_EDX]
        mov     [ebp + F_EDX], eax
        mov     eax, [rm_req + RC_ESI]
        mov     [ebp + F_ESI], eax
        mov     eax, [rm_req + RC_EDI]
        mov     [ebp + F_EDI], eax
        mov     eax, [rm_req + RC_EBP]
        mov     [ebp + F_EBP], eax
        movzx   eax, word [rm_req + RC_FLAGS]
        and     eax, 0x0CD5                     ; CF PF AF ZF SF OF DF
        and     dword [ebp + F_EFL], ~0x0CD5
        or      [ebp + F_EFL], eax
        ret

; =============================================================================
; deliver_int: run the client's protected-mode handler for vector EAX.
;   Its frame goes on the client's own stack: the interrupted context first,
;   then an IRET frame that returns into our ring-3 stub.
; =============================================================================
deliver_int:
        mov     esi, [pm_vectors + eax*8]       ; offset
        movzx   edx, word [pm_vectors + eax*8 + 4]  ; selector
        movzx   ecx, word [ebp + F_SS]
        mov     eax, [ebp + F_ESP]
        call    stack_addr                      ; EAX = linear top, EDX = mask
        mov     edi, eax
        sub     edi, 32                         ; linear address of the new top
        mov     eax, [ebp + F_ESP]
        mov     [gs:edi + 24], eax              ; ESP as it was
        sub     eax, 32
        call    frame_set_esp
        mov     eax, [ebp + F_EIP]
        mov     [gs:edi + 12], eax
        mov     eax, [ebp + F_CS]
        mov     [gs:edi + 16], eax
        mov     eax, [ebp + F_EFL]
        mov     [gs:edi + 20], eax
        mov     eax, [ebp + F_SS]
        mov     [gs:edi + 28], eax
        mov     dword [gs:edi + 0], ret_stub_int
        mov     dword [gs:edi + 4], SEL_UCODE32
        mov     eax, [ebp + F_EFL]
        and     eax, ~EFL_TF
        mov     [gs:edi + 8], eax               ; what the handler's IRET pops
        ; the handler itself, with interrupts off
        movzx   eax, byte [ebp + F_VEC]
        mov     esi, [pm_vectors + eax*8]
        movzx   edx, word [pm_vectors + eax*8 + 4]
        mov     [ebp + F_EIP], esi
        mov     [ebp + F_CS], edx
        and     dword [ebp + F_EFL], ~(EFL_IF | EFL_TF)
        jmp     int_exit

; =============================================================================
; exception: vector 0-31, not a hardware request
; =============================================================================
exception:
        cmp     eax, 13
        jne     .not_gp
        call    gp_emulate                      ; CLI, STI, HLT and port traps
        jnc     int_exit                        ; handled
.not_gp:
        movzx   eax, byte [ebp + F_VEC]
        cmp     dword [exc_handlers + eax*8], 0
        jne     deliver_exc
        cmp     dword [pm_vectors + eax*8], 0
        jne     deliver_int
        ; ---- nobody wants it: end the program, saying where ----
        mov     [fault_vec], al
        mov     eax, [ebp + F_EIP]
        mov     [fault_eip], eax
        mov     eax, [ebp + F_CS]
        mov     [fault_cs], eax
        mov     eax, [ebp + F_ERR]
        mov     [fault_err], eax
        mov     byte [rm_kind], 4
        call    depart                          ; does not come back

; host_fault: an exception with a ring-0 CS: our own bug.  Report it.
host_fault:
        movzx   eax, byte [ebp + F_VEC]
        or      al, 0x80                        ; marked as ours
        mov     [fault_vec], al
        mov     eax, [ebp + F_EIP]
        mov     [fault_eip], eax
        mov     eax, [ebp + F_CS]
        mov     [fault_cs], eax
        mov     eax, [ebp + F_ERR]
        mov     [fault_err], eax
        mov     byte [rm_kind], 4
        mov     esp, [r0_top]                   ; whatever the stack was
        sub     esp, 0x100                      ;  (the frame stays readable)
        call    depart

; =============================================================================
; deliver_exc: the client's exception handler, on the locked stack
;   Frame, from the top: previous locked_sp, SS, ESP, EFLAGS, CS, EIP, error,
;   then the far return into our stub.  The handler returns with RETF.
; =============================================================================
deliver_exc:
        mov     esi, [exc_handlers + eax*8]
        movzx   edx, word [exc_handlers + eax*8 + 4]
        mov     edi, [locked_sp]
        sub     edi, 36
        mov     eax, [locked_sp]
        mov     [gs:edi + 32], eax              ; previous locked_sp
        mov     [locked_sp], edi
        mov     eax, [ebp + F_SS]
        mov     [gs:edi + 28], eax
        mov     eax, [ebp + F_ESP]
        mov     [gs:edi + 24], eax
        mov     eax, [ebp + F_EFL]
        mov     [gs:edi + 20], eax
        mov     eax, [ebp + F_CS]
        mov     [gs:edi + 16], eax
        mov     eax, [ebp + F_EIP]
        mov     [gs:edi + 12], eax
        mov     eax, [ebp + F_ERR]
        mov     [gs:edi + 8], eax
        mov     dword [gs:edi + 4], SEL_UCODE32
        mov     dword [gs:edi + 0], ret_stub_exc
        ; SS:ESP = the locked stack, at the frame
        movzx   eax, word [locked_sel]
        mov     [ebp + F_SS], eax
        mov     eax, edi
        sub     eax, [xarea]
        sub     eax, XA_LOCKED
        mov     [ebp + F_ESP], eax
        mov     [ebp + F_EIP], esi
        mov     [ebp + F_CS], edx
        and     dword [ebp + F_EFL], ~(EFL_IF | EFL_TF)
        inc     dword [exc_count]
        jmp     int_exit

; =============================================================================
; the ring-3 stubs (SEL_UCODE32, base = this segment), and what they lead to
; =============================================================================
ret_stub_int:   int     RET_VECTOR
ret_stub_exc:   int     RET_VECTOR
ret_stub_cb:    int     RET_VECTOR
raw_pm_stub:    int     RAW_VECTOR              ; jumped to: switch to real mode

; raw_dispatch: the client jumped to raw_pm_stub with AX = DS, CX = ES,
;   DX = SS, EBX = ESP, SI = CS, EDI = EIP, all real-mode values.  Real
;   mode goes on there; nothing of this side is kept.
raw_dispatch:
        mov     eax, [ebp + F_EIP]
        cmp     eax, raw_pm_stub + 2
        jne     sw_interrupt                    ; somebody's own INT F7h
        call    frame_to_req
        mov     eax, [ebp + F_EAX]
        mov     [rm_req + RC_DS], ax
        mov     eax, [ebp + F_ECX]
        mov     [rm_req + RC_ES], ax
        mov     eax, [ebp + F_EDX]
        mov     [rm_req + RC_SS], ax
        mov     eax, [ebp + F_EBX]
        mov     [rm_req + RC_SP], ax
        mov     eax, [ebp + F_ESI]
        mov     [rm_req + RC_CS], ax
        mov     eax, [ebp + F_EDI]
        mov     [rm_req + RC_IP], ax
        mov     word [rm_req + RC_FS], 0
        mov     word [rm_req + RC_GS], 0
        mov     word [rm_req + RC_FLAGS], 0x0002 ; interrupts stay off
        mov     byte [rm_kind], 5
        jmp     depart_resume

; raw_pm_enter: real-mode code came through raw_rm_entry.  Its registers
;   are in cb_regs: AX = DS, CX = ES, DX = SS, EBX = ESP, SI = CS, EDI = EIP.
raw_pm_enter:
        call    set_esp0
        movzx   eax, word [cb_regs + RC_EDX]
        push    eax                             ; SS
        push    dword [cb_regs + RC_EBX]         ; ESP
        push    dword 0x0002                    ; EFLAGS: interrupts off
        movzx   eax, word [cb_regs + RC_ESI]
        push    eax                             ; CS
        push    dword [cb_regs + RC_EDI]         ; EIP
        mov     ax, [cb_regs + RC_ECX]
        mov     es, ax
        xor     eax, eax
        mov     fs, ax
        mov     gs, ax
        mov     ebx, [cb_regs + RC_EBX]
        mov     ecx, [cb_regs + RC_ECX]
        mov     edx, [cb_regs + RC_EDX]
        mov     esi, [cb_regs + RC_ESI]
        mov     edi, [cb_regs + RC_EDI]
        mov     ebp, [cb_regs + RC_EBP]
        mov     eax, [cb_regs + RC_EAX]
        mov     ds, ax
        iretd

ret_from_client:
        mov     eax, [ebp + F_EIP]
        cmp     eax, ret_stub_int + 2
        je      .from_int
        cmp     eax, ret_stub_exc + 2
        je      .from_exc
        cmp     eax, ret_stub_cb + 2
        je      .from_cb
        jmp     sw_interrupt                    ; somebody's own INT F9h
.from_int:
        ; the interrupted context sits at the client's ESP
        movzx   ecx, word [ebp + F_SS]
        mov     eax, [ebp + F_ESP]
        call    stack_addr
        mov     esi, eax
        mov     eax, [gs:esi + 0]
        mov     [ebp + F_EIP], eax
        mov     eax, [gs:esi + 4]
        mov     [ebp + F_CS], eax
        ; the flags: what the handler's IRET handed back (DOS answers with
        ; CF in the frame), with the interrupt flag as it was before
        mov     eax, [ebp + F_EFL]
        and     eax, ~(EFL_IF | EFL_TF | EFL_NT | EFL_IOPL)
        mov     ecx, [gs:esi + 8]
        and     ecx, EFL_IF | EFL_TF
        or      eax, ecx
        mov     [ebp + F_EFL], eax
        mov     eax, [gs:esi + 12]
        mov     [ebp + F_ESP], eax
        mov     eax, [gs:esi + 16]
        mov     [ebp + F_SS], eax
        jmp     int_exit
.from_exc:
        ; ESP -> error code, then EIP CS EFLAGS ESP SS, previous locked_sp
        mov     esi, [ebp + F_ESP]
        add     esi, [xarea]
        add     esi, XA_LOCKED
        mov     eax, [gs:esi + 4]
        mov     [ebp + F_EIP], eax
        mov     eax, [gs:esi + 8]
        mov     [ebp + F_CS], eax
        mov     eax, [gs:esi + 12]
        or      eax, EFL_IF
        and     eax, ~(EFL_NT | EFL_IOPL)
        mov     [ebp + F_EFL], eax
        mov     eax, [gs:esi + 16]
        mov     [ebp + F_ESP], eax
        mov     eax, [gs:esi + 20]
        mov     [ebp + F_SS], eax
        mov     eax, [gs:esi + 24]
        mov     [locked_sp], eax
        jmp     int_exit
.from_cb:
        ; ESP -> the callback number, the previous locked_sp
        mov     esi, [ebp + F_ESP]
        add     esi, [xarea]
        add     esi, XA_LOCKED
        mov     ecx, [gs:esi + 0]
        mov     eax, [gs:esi + 4]
        mov     [locked_sp], eax
        ; the structure, as the client left it, becomes the real-mode state
        imul    ecx, CB_ENTRY
        movzx   edx, word [cb_table + ecx + CB_RMCS_SEL]
        push    ecx
        mov     ecx, edx
        call    desc_base
        pop     ecx
        add     eax, [cb_table + ecx + CB_RMCS]
        mov     esi, eax
        mov     edi, rm_req
        push    ds
        pop     es                              ; ES = host data for the copy
        mov     ecx, RC_SIZE / 2
        push    ds
        push    gs
        pop     ds                              ; DS = flat: source
        rep     movsw
        pop     ds
        mov     ax, SEL_FLAT
        mov     es, ax
        mov     byte [rm_kind], 5
        jmp     depart_resume

; =============================================================================
; callback_enter: real-mode code called callback cb_index.  Registers are in
;   cb_regs.  Put them in the client's structure and call its procedure on
;   the locked stack with DS:ESI -> the structure, ES:EDI -> the real-mode
;   stack.  The procedure ends with IRET into our stub.
; =============================================================================
callback_enter:
        movzx   ecx, word [cb_index]
        imul    ecx, CB_ENTRY
        movzx   edx, word [cb_table + ecx + CB_RMCS_SEL]
        push    ecx
        mov     ecx, edx
        call    desc_base
        pop     ecx
        add     eax, [cb_table + ecx + CB_RMCS]
        mov     edi, eax
        mov     esi, cb_regs
        push    ecx
        mov     ecx, RC_SIZE / 2
        rep     movsw                           ; DS:ESI (host) -> ES:EDI (flat)
        pop     ecx
        ; a selector on the real-mode stack
        movzx   ebx, word [cb_table + ecx + CB_STACK_SEL]
        movzx   eax, word [cb_regs + RC_SS]
        shl     eax, 4
        push    ecx
        mov     ecx, 0xFFFF
        call    desc_set_base_limit
        pop     ecx
        ; the locked-stack frame: the number, the old locked_sp, then IRET's
        mov     edi, [locked_sp]
        sub     edi, 20
        mov     eax, [locked_sp]
        mov     [gs:edi + 16], eax
        mov     [locked_sp], edi
        movzx   eax, word [cb_index]
        mov     [gs:edi + 12], eax              ; the callback number
        movzx   eax, word [cb_regs + RC_FLAGS]
        and     eax, ~(EFL_IF | EFL_TF | EFL_NT | EFL_IOPL)
        mov     [gs:edi + 8], eax
        mov     dword [gs:edi + 4], SEL_UCODE32
        mov     dword [gs:edi + 0], ret_stub_cb
        ; ---- into the procedure ----
        push    dword [locked_sel]              ; SS
        mov     eax, edi
        sub     eax, [xarea]
        sub     eax, XA_LOCKED
        push    eax                             ; ESP
        push    dword 0x0002                    ; EFLAGS: interrupts off
        movzx   eax, word [cb_table + ecx + CB_PROC_SEL]
        push    eax                             ; CS
        push    dword [cb_table + ecx + CB_PROC] ; EIP
        mov     esi, [cb_table + ecx + CB_RMCS]
        movzx   edi, word [cb_regs + RC_SP]
        mov     ax, [cb_table + ecx + CB_STACK_SEL]
        mov     es, ax
        mov     ax, [cb_table + ecx + CB_RMCS_SEL]
        xor     ebx, ebx
        mov     fs, bx
        mov     gs, bx
        mov     ds, ax
        iretd

; =============================================================================
; departures and arrivals
; =============================================================================
; depart: run rm_kind in real mode and come back here (as a return).  The
;   ring-0 stack is remembered per nesting level, and ESP0 is lowered so an
;   interrupt taken while a callback's procedure runs cannot land on it.
depart:
        push    ebp
        push    ebx
        push    esi
        push    edi
        movzx   eax, byte [nest_depth]
        mov     [nest_esp + eax*4], esp
        inc     byte [nest_depth]
        mov     [tss + 4], esp
        jmp     SEL_HCODE16:leave_pm

; depart_resume: real mode continues from rm_req; nothing pending returns here
depart_resume:
        jmp     SEL_HCODE16:leave_pm

; pm_reenter: from real mode, on the host's ring-0 stack again
pm_reenter:
        mov     ax, SEL_HDATA32
        mov     ds, ax
        mov     ax, SEL_FLAT
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        cld
        cmp     byte [pm_entry_kind], 0
        je      .returning
        cmp     byte [pm_entry_kind], 2
        je      .raw
        jmp     .callback
.raw:   movzx   eax, byte [nest_depth]
        dec     eax
        js      .raw_top
        mov     esp, [nest_esp + eax*4]
        FLAGS_KNOWN
        jmp     raw_pm_enter
.raw_top:
        mov     esp, [r0_top]
        FLAGS_KNOWN
        jmp     raw_pm_enter
.returning:
        dec     byte [nest_depth]
        movzx   eax, byte [nest_depth]
        mov     esp, [nest_esp + eax*4]
        FLAGS_KNOWN
        call    set_esp0
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret                                     ; to the caller of depart
.callback:
        movzx   eax, byte [nest_depth]
        dec     eax
        js      .no_excursion
        mov     esp, [nest_esp + eax*4]
        FLAGS_KNOWN
        jmp     callback_enter
.no_excursion:
        mov     esp, [r0_top]                   ; a callback with the client in
        FLAGS_KNOWN                             ;  real mode: nothing pending
        jmp     callback_enter

; set_esp0: the ring-0 stack an interrupt from ring 3 must use
set_esp0:
        movzx   eax, byte [nest_depth]
        or      eax, eax
        jz      .top
        dec     eax
        mov     eax, [nest_esp + eax*4]
        mov     [tss + 4], eax
        ret
.top:   mov     eax, [r0_top]
        mov     [tss + 4], eax
        ret

; =============================================================================
; gp_emulate: a general protection fault may be CLI, STI, HLT or port I/O
;   from ring 3.  CF=0 if it was handled.
; =============================================================================
gp_emulate:
        cmp     dword [ebp + F_ERR], 0          ; a selector error is a real fault
        jne     .real
        movzx   ecx, word [ebp + F_CS]
        call    desc_base
        add     eax, [ebp + F_EIP]
        mov     esi, eax
        mov     al, [gs:esi]
        cmp     al, 0xFA                        ; CLI
        je      .cli
        cmp     al, 0xFB                        ; STI
        je      .sti
        cmp     al, 0xF4                        ; HLT: just carry on
        je      .skip1
        call    io_emulate                      ; IN, OUT with a watched port
        jc      .real
        ret
.cli:   and     dword [ebp + F_EFL], ~EFL_IF
        jmp     .skip1
.sti:   or      dword [ebp + F_EFL], EFL_IF
.skip1: inc     dword [ebp + F_EIP]
        clc
        ret
.real:  stc
        ret

; io_emulate: the instruction at GS:ESI is IN or OUT on a port the bitmap
;   traps.  Nothing is trapped yet, so nothing is emulated.  CF=1.
io_emulate:
        stc
        ret

; =============================================================================
; the client's stack: a 16-bit stack segment uses SP, whatever ESP holds
; =============================================================================
; stack_addr: ECX = SS selector, EAX = ESP -> EAX = the linear address of
;   the top, EDX = the mask that applies to ESP (FFFFh or FFFFFFFFh)
stack_addr:
        push    esi
        push    eax
        call    desc_addr
        mov     edx, 0xFFFFFFFF
        test    byte [gs:esi + 6], 0x40         ; the big bit
        jnz     .big
        mov     edx, 0xFFFF
.big:   call    desc_base
        pop     esi
        and     esi, edx
        add     eax, esi
        pop     esi
        ret

; frame_set_esp: the client's ESP in the frame := EAX, under mask EDX
frame_set_esp:
        push    ecx
        mov     ecx, [ebp + F_ESP]
        not     edx
        and     ecx, edx
        not     edx
        and     eax, edx
        or      eax, ecx
        mov     [ebp + F_ESP], eax
        pop     ecx
        ret

; =============================================================================
; descriptors
; =============================================================================
; desc_base: ECX = selector -> EAX = its base.  A GDT selector is looked up
;   in our own table; an LDT one in the client's.
desc_base:
        push    esi
        call    desc_addr                       ; ESI = linear address of it
        jc      .zero
        push    edx
        movzx   eax, word [gs:esi + 2]
        movzx   edx, byte [gs:esi + 4]
        shl     edx, 16
        or      eax, edx
        movzx   edx, byte [gs:esi + 7]
        shl     edx, 24
        or      eax, edx
        pop     edx
        pop     esi
        ret
.zero:  xor     eax, eax
        pop     esi
        ret

; desc_addr: ECX = selector -> ESI = linear address of the descriptor.
;   CF=1 if the selector is out of range.
desc_addr:
        push    eax
        mov     eax, ecx
        and     eax, 0xFFFF
        test    al, 4
        jz      .gdt
        and     eax, ~7
        cmp     eax, LDT_ENTRIES * 8
        jae     .bad
        add     eax, [ldt_lin]
        mov     esi, eax
        pop     eax
        clc
        ret
.gdt:   and     eax, ~7
        cmp     eax, GDT_ENTRIES * 8
        jae     .bad
        add     eax, [host_base]
        add     eax, gdt
        mov     esi, eax
        pop     eax
        clc
        ret
.bad:   pop     eax
        stc
        ret

; desc_valid: ECX = selector -> CF=0 if it is an allocated LDT descriptor
desc_valid:
        push    eax
        push    edx
        mov     eax, ecx
        and     eax, 0xFFFF
        test    al, 4
        jz      .bad
        shr     eax, 3
        cmp     eax, LDT_ENTRIES
        jae     .bad
        bt      [ldt_used], eax
        jnc     .bad
        pop     edx
        pop     eax
        clc
        ret
.bad:   pop     edx
        pop     eax
        stc
        ret

; desc_new: a fresh LDT descriptor.  EAX = base, ECX = limit (byte
;   granular, < 1 MB), DL = access byte -> BX = selector.  CF=1 if none.
;   Both come back as they went in.  They did not once: the routine took
;   the limit apart with SHR in place, so a second call that let the first
;   one's limit stand got a segment of one byte, and the client faulted on
;   its first write to DS.  QEMU never noticed - it does not enforce the
;   limit of a data segment.
desc_new:
        push    eax
        push    ecx
        mov     ecx, 1
        call    ldt_alloc                       ; -> EBX = first index
        jc      .none
        shl     ebx, 3
        or      ebx, 7
        mov     ecx, ebx
        call    desc_addr
        mov     eax, [esp + 4]                  ; the base and the limit again
        mov     ecx, [esp]
        mov     [gs:esi + 2], ax
        shr     eax, 16
        mov     [gs:esi + 4], al
        mov     [gs:esi + 7], ah
        mov     [gs:esi], cx
        shr     ecx, 16
        and     cl, 0x0F
        mov     [gs:esi + 6], cl
        mov     [gs:esi + 5], dl
        pop     ecx
        pop     eax
        call    desc_check
        clc
        ret
.none:  pop     ecx
        pop     eax
        stc
        ret

; desc_set_32: BX = selector: make it a 32-bit segment (D/B set)
desc_set_32:
        push    ecx
        push    esi
        mov     ecx, ebx
        call    desc_addr
        or      byte [gs:esi + 6], 0x40
        pop     esi
        pop     ecx
        ret

; desc_set_base_limit: BX = selector, EAX = base, ECX = limit (byte granular).
;   EAX, ECX and ESI come back as they went in.  The pops used to run in the
;   same order as the pushes, so the limit written was whatever ESI held on
;   entry - and the one caller arrives straight out of a REP MOVSW.
desc_set_base_limit:
        push    ecx                             ; the limit
        push    esi
        push    eax                             ; the base
        mov     ecx, ebx
        call    desc_addr
        mov     eax, [esp]                      ; the base again
        jc      .out
        mov     ecx, [esp + 8]                  ; the limit again
        mov     [gs:esi + 2], ax
        shr     eax, 16
        mov     [gs:esi + 4], al
        mov     [gs:esi + 7], ah
        mov     [gs:esi], cx
        shr     ecx, 16
        and     cl, 0x0F
        and     byte [gs:esi + 6], 0xF0
        or      [gs:esi + 6], cl
        mov     eax, [esp]                      ; as they were asked for
        mov     ecx, [esp + 8]
        call    desc_check
.out:   pop     eax
        pop     esi
        pop     ecx
        ret

; desc_check: BX = selector, EAX = base, ECX = limit, as they were asked for.
;   Reads the descriptor back through the same path that wrote it and stops
;   the client if it does not say what it was told to.  A descriptor is
;   written once and only the processor reads it after that, and QEMU
;   enforces neither a data segment's limit nor much else about one, so a
;   routine that scrambles a register on the way is invisible there until
;   real silicon faults on the client.  Three of those cost a trip each.
;   Only the host's own descriptors come through here: they are all byte
;   granular, so a page-granular one is wrong by definition.
desc_check:
        push    eax
        push    ecx
        push    edx
        push    esi
        mov     [dc_base], eax
        mov     [dc_limit], ecx
        mov     [dc_sel], bx
        mov     dword [dc_got_base], -1
        mov     dword [dc_got_limit], -1
        movzx   ecx, bx
        call    desc_addr
        jc      .bad
        movzx   eax, word [gs:esi + 2]          ; the base, reassembled
        movzx   edx, byte [gs:esi + 4]
        shl     edx, 16
        or      eax, edx
        movzx   edx, byte [gs:esi + 7]
        shl     edx, 24
        or      eax, edx
        mov     [dc_got_base], eax
        movzx   eax, word [gs:esi]              ; the limit, with its high nibble
        movzx   ecx, byte [gs:esi + 6]
        mov     edx, ecx
        and     edx, 0x0F
        shl     edx, 16
        or      eax, edx
        mov     [dc_got_limit], eax
        test    cl, 0x80                        ; page granular: not one of ours
        jnz     .bad
        cmp     eax, [dc_limit]
        jne     .bad
        mov     eax, [dc_got_base]
        cmp     eax, [dc_base]
        jne     .bad
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret
.bad:   mov     byte [rm_kind], 6
        mov     esp, [r0_top]                   ; whatever the stack was
        sub     esp, 0x100
        call    depart                          ; does not come back

; ldt_alloc: ECX consecutive free entries -> EBX = the first.  CF=1 if none.
ldt_alloc:
        push    eax
        push    edx
        push     esi
        xor     ebx, ebx                        ; candidate start
.try:   mov     eax, ebx
        add     eax, ecx
        cmp     eax, LDT_ENTRIES
        ja      .none
        xor     edx, edx                        ; how many free so far
.run:   mov     eax, ebx
        add     eax, edx
        bt      [ldt_used], eax
        jc      .taken
        inc     edx
        cmp     edx, ecx
        jb      .run
        ; mark them
        xor     edx, edx
.mark:  mov     eax, ebx
        add     eax, edx
        bts     [ldt_used], eax
        inc     edx
        cmp     edx, ecx
        jb      .mark
        pop     esi
        pop     edx
        pop     eax
        clc
        ret
.taken: lea     ebx, [ebx + edx + 1]
        jmp     .try
.none:  pop     esi
        pop     edx
        pop     eax
        stc
        ret

; ldt_free: ECX = selector.  The entry goes back and is cleared.  A segment
;   register of the caller's still holding it is zeroed, or the return to
;   the client would fault loading it (EBP = the frame).
ldt_free:
        push    eax
        push    esi
        push    edx
        call    desc_valid
        jc      .out
        mov     eax, ecx
        and     eax, 0xFFFF
        shr     eax, 3
        btr     [ldt_used], eax
        call    desc_addr
        mov     dword [gs:esi], 0
        mov     dword [gs:esi + 4], 0
        mov     eax, ecx
        and     eax, 0xFFFC
        mov     esi, F_DS
.regs:  mov     edx, [ebp + esi]
        and     edx, 0xFFFC
        cmp     edx, eax
        jne     .next
        mov     dword [ebp + esi], 0
.next:  sub     esi, 4
        jns     .regs                           ; F_DS, F_ES, F_FS, F_GS
.out:   pop     edx
        pop     esi
        pop     eax
        ret

section .data
                align 4
ldt_lin:        dd 0
cl_cs:          dd 0
cl_ds:          dd 0
cl_ss:          dd 0
cl_psp:         dd 0
locked_sel:     dd 0
psp_env_seg:    dw 0
section .text
