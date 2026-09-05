; =============================================================================
;  pm32.asm - the 32-bit application runtime ("NX32" programs)
; -----------------------------------------------------------------------------
;  An NX32 file is a flat 32-bit image with a 32-byte header:
;     +0  "NX32"   +4 load address   +8 entry   +12 end of data (image end)
;     +16 end of bss   +20 stack size   (rest reserved)
;  It is loaded above 1 MB, the CPU is switched to protected mode (flat, ring
;  0) and the program is entered with EBX -> the app_info block.  Hardware
;  interrupts (timer, keyboard) are delivered to handlers the program
;  registers in app_info.  The program calls the kernel with INT 80h and
;  EBX -> an rmcall block: the runtime drops back to real mode, executes the
;  requested BIOS/DOS interrupt with those registers, and returns.
;
;  While in protected mode the PIC is remapped to vectors 20h-2Fh; on every
;  excursion to real mode it is put back to 08h/70h so the BIOS keeps working.
; =============================================================================

KBASE           equ 0x8000                      ; physical address of the kernel
SEL_KCODE32     equ 0x08                        ; 32-bit code, base KBASE
SEL_KDATA32     equ 0x10                        ; 32-bit data, flat
SEL_KCODE16     equ 0x18                        ; 16-bit code, base KBASE
SEL_KDATA16     equ 0x20                        ; 16-bit data, base KBASE
SEL_ACODE32     equ 0x28                        ; 32-bit code, flat (the app)
NX_MIN_LOAD     equ 0x100000
NX_IDT_ENTRIES  equ 0x81                        ; exceptions, the IRQs and INT 80h
NX_BOUNCE_PARAS equ 0x1000                      ; 64 KB real-mode bounce buffer

; rmcall block (in low memory, built by the app)
RC_AX           equ 0
RC_BX           equ 2
RC_CX           equ 4
RC_DX           equ 6
RC_SI           equ 8
RC_DI           equ 10
RC_DS           equ 12
RC_ES           equ 14
RC_FLAGS        equ 16
RC_INT          equ 18                          ; interrupt number; FFh = exit, FEh = panic
RC_BP           equ 20                          ; some BIOS calls answer in BP

; app_info block (kernel memory, handed to the program)
AI_MAGIC        equ 0
AI_MEM_START    equ 4
AI_MEM_END      equ 8
AI_BOUNCE       equ 12
AI_BOUNCE_SIZE  equ 16
AI_CMDLINE      equ 20
AI_IRQ0         equ 24                          ; filled by the app: far-return stubs
AI_IRQ1         equ 28
AI_IRQ12        equ 32                          ; the mouse, for a graphical app
AI_SIZE         equ 36

; -----------------------------------------------------------------------------
; run_nx32: fstream is open at the file's start (found_* set).  Loads and runs
;   the program; returns CF=0 when it has finished, CF=1 (AX = DOS error) if
;   it could not be started.
; -----------------------------------------------------------------------------
run_nx32:
        mov     [nx_ss], ss
        mov     [nx_sp], sp
        ; ---- header ----
        call    fstream_open
        push    cs
        pop     es
        mov     di, nx_hdr
        mov     ecx, 32
        call    fstream_read
        cmp     dword [nx_hdr], "NX32"
        jne     .bad
        mov     eax, [nx_hdr+4]
        cmp     eax, NX_MIN_LOAD
        jb      .bad
        mov     [nx_load], eax
        mov     eax, [nx_hdr+12]
        sub     eax, [nx_load]
        mov     [nx_image_size], eax
        ; ---- extended memory size (INT 15h E801h) ----
        call    ext_mem_end                     ; EAX = end of usable memory
        mov     [nx_mem_end], eax
        mov     eax, [nx_hdr+16]
        add     eax, [nx_hdr+20]
        add     eax, 15
        and     eax, ~15
        mov     [nx_stack_top], eax
        mov     [nx_mem_start], eax
        cmp     eax, [nx_mem_end]
        jae     .no_memory
        ; ---- bounce buffer from the arena ----
        mov     bx, NX_BOUNCE_PARAS
        call    mem_alloc
        jc      .no_memory
        mov     [nx_bounce_seg], ax
        ; ---- load the image above 1 MB, 64 KB at a time ----
        call    enter_unreal
        mov     edi, [nx_load]
        add     edi, 32                         ; the header is part of the image
        mov     ecx, [nx_image_size]
        sub     ecx, 32
        mov     [nx_remaining], ecx
.chunk: cmp     dword [nx_remaining], 0
        je      .loaded
        mov     ecx, [nx_remaining]
        cmp     ecx, 0x8000                     ; 32 KB per read (16-bit counts inside)
        jbe     .chunk_size
        mov     ecx, 0x8000
.chunk_size:
        mov     [nx_chunk], ecx
        mov     es, [nx_bounce_seg]
        xor     di, di
        call    fstream_read                    ; into the bounce buffer
        push    cs
        pop     es
        ; copy bounce -> destination with 32-bit addressing through GS (flat)
        mov     edi, [nx_load]                  ; (DI was used by the read)
        add     edi, [nx_image_size]
        sub     edi, [nx_remaining]
        movzx   esi, word [nx_bounce_seg]
        shl     esi, 4
        mov     ecx, [nx_chunk]
        add     ecx, 3
        shr     ecx, 2
.copy:  mov     eax, [gs:esi]
        mov     [gs:edi], eax
        add     esi, 4
        add     edi, 4
        dec     ecx
        jnz     .copy
        mov     eax, [nx_chunk]
        sub     [nx_remaining], eax
        ; keep EDI exact (the copy rounded up to a dword)
        mov     edi, [nx_load]
        add     edi, [nx_image_size]
        sub     edi, [nx_remaining]
        jmp     .chunk
.loaded:
        ; ---- zero the bss ----
        mov     edi, [nx_hdr+12]
        mov     ecx, [nx_hdr+16]
        sub     ecx, edi
        jbe     .bss_done
.zero:  mov     byte [gs:edi], 0
        inc     edi
        dec     ecx
        jnz     .zero
.bss_done:
        ; ---- app_info ----
        mov     dword [app_info+AI_MAGIC], "NXI1"
        mov     eax, [nx_mem_start]
        mov     [app_info+AI_MEM_START], eax
        mov     eax, [nx_mem_end]
        mov     [app_info+AI_MEM_END], eax
        movzx   eax, word [nx_bounce_seg]
        shl     eax, 4
        mov     [app_info+AI_BOUNCE], eax
        mov     dword [app_info+AI_BOUNCE_SIZE], NX_BOUNCE_PARAS * 16
        mov     eax, KBASE + exec_tail
        mov     [app_info+AI_CMDLINE], eax
        mov     dword [app_info+AI_IRQ0], 0
        mov     dword [app_info+AI_IRQ1], 0
        mov     dword [app_info+AI_IRQ12], 0
        ; the command tail as a NUL-terminated string
        movzx   bx, byte [exec_tail_len]
        mov     byte [exec_tail+bx], 0
        ; ---- descriptor tables ----
        call    build_idt
        mov     eax, KBASE + gdt32
        mov     [gdtr32+2], eax
        mov     eax, KBASE + idt32
        mov     [idtr32+2], eax
        ; ---- interrupt controller: save masks, remap for protected mode ----
        in      al, 0x21
        mov     [nx_pic1_mask], al
        in      al, 0xA1
        mov     [nx_pic2_mask], al
        mov     byte [nx_running], 1
        call    kb_flush
        ; ---- go ----
        cli
        call    pic_remap_pm
        lgdt    [gdtr32]
        lidt    [idtr32]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     SEL_KCODE32:pm_start
.bad:   mov     ax, 11                          ; invalid format
        stc
        ret
.no_memory:
        mov     ax, 8
        stc
        ret

; =============================================================================
; 32-bit side (code segment based at KBASE: labels are usable as EIPs)
; =============================================================================
[BITS 32]
pm_start:
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     esp, [KBASE + nx_stack_top]
        ; FPU on (Doom uses x87 in a few places)
        mov     eax, cr0
        and     eax, ~0x0C                      ; EM=0, TS=0
        or      eax, 0x22                       ; MP=1, NE=1
        mov     cr0, eax
        fninit
        mov     ebx, KBASE + app_info
        xor     eax, eax
        xor     ecx, ecx
        xor     edx, edx
        xor     esi, esi
        xor     edi, edi
        xor     ebp, ebp
        sti
        push    dword SEL_ACODE32
        push    dword [KBASE + nx_hdr + 8]      ; entry
        retf

; ---- INT 80h: run a real-mode interrupt for the program -------------------
syscall_stub:
        pushad
        push    ds
        push    es
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     [KBASE + sys_req], ebx
syscall_common:
        mov     [KBASE + pm_esp], esp
        cli
        jmp     SEL_KCODE16:to_real_mode

; ---- IRQ stubs ---------------------------------------------------------------
irq0_stub:
        pushad
        push    ds
        push    es
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     eax, [KBASE + app_info + AI_IRQ0]
        test    eax, eax
        jz      .eoi
        mov     [KBASE + far_ptr], eax
        mov     word [KBASE + far_ptr + 4], SEL_ACODE32
        call    far dword [KBASE + far_ptr]
.eoi:   mov     al, 0x20
        out     0x20, al
        pop     es
        pop     ds
        popad
        iretd

irq1_stub:
        pushad
        push    ds
        push    es
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     eax, [KBASE + app_info + AI_IRQ1]
        test    eax, eax
        jnz     .call
        in      al, 0x60                        ; nobody wants it: drain
        jmp     .eoi
.call:  mov     [KBASE + far_ptr], eax
        mov     word [KBASE + far_ptr + 4], SEL_ACODE32
        call    far dword [KBASE + far_ptr]
.eoi:   mov     al, 0x20
        out     0x20, al
        pop     es
        pop     ds
        popad
        iretd

irq12_stub:                                     ; the PS/2 mouse
        pushad
        push    ds
        push    es
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     eax, [KBASE + app_info + AI_IRQ12]
        test    eax, eax
        jnz     .call
        in      al, 0x60                        ; nobody wants it: drain
        jmp     .eoi
.call:  mov     [KBASE + far_ptr], eax
        mov     word [KBASE + far_ptr + 4], SEL_ACODE32
        call    far dword [KBASE + far_ptr]
.eoi:   mov     al, 0x20
        out     0xA0, al                        ; the slave first, then the master
        out     0x20, al
        pop     es
        pop     ds
        popad
        iretd

irq_master_stub:                                ; other master IRQs: just EOI
        push    eax
        mov     al, 0x20
        out     0x20, al
        pop     eax
        iretd

irq_slave_stub:
        push    eax
        mov     al, 0x20
        out     0xA0, al
        out     0x20, al
        pop     eax
        iretd

ignore_stub:
        iretd

; ---- exceptions: record and panic ---------------------------------------------
%macro EXC_NOERR 1
exc_%1: push    dword %1
        jmp     exc_common
%endmacro
%macro EXC_ERR 1
exc_%1: pop     dword [KBASE + exc_err]         ; the error code
        push    dword %1
        jmp     exc_common
%endmacro
EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
exc_common:
        pop     dword [KBASE + exc_vec]
        mov     eax, [esp]
        mov     [KBASE + exc_eip], eax
        mov     eax, [esp+4]
        mov     [KBASE + exc_cs], eax
        mov     [KBASE + exc_esp], esp
        pushad
        push    ds
        push    es
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     dword [KBASE + sys_req], KBASE + panic_req
        jmp     syscall_common

; ---- back from real mode ------------------------------------------------------
pm_return:
        mov     ax, SEL_KDATA32
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     esp, [KBASE + pm_esp]
        ; A key pressed during the excursion sat in the keyboard controller
        ; with its interrupt masked, and re-initialising the controller just
        ; now forgot the request.  The chip will not ask again until the byte
        ; is read, so deliver it ourselves or the keyboard is dead from here.
        in      al, 0x64
        test    al, 0x01
        jz      .no_key
        int     0x21
.no_key:
        pop     es
        pop     ds
        popad
        iretd

; =============================================================================
; 16-bit protected-mode / real-mode transitions
; =============================================================================
[BITS 16]
to_real_mode:                                   ; CS = SEL_KCODE16 here
        mov     ax, SEL_KDATA16
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     eax, cr0
        and     al, 0xFE
        mov     cr0, eax
        jmp     KERNEL_SEG:rm_entry
rm_entry:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, nx_rm_stack_top
        lidt    [idtr_real]
        call    pic_remap_rm
        mov     al, [nx_pic1_mask]
        or      al, 0x02                        ; keyboard stays masked for the app
        out     0x21, al
        mov     al, [nx_pic2_mask]
        out     0xA1, al
        sti
        call    do_rm_request                   ; does not return for exit / panic
        cli
        call    pic_remap_pm
        lgdt    [gdtr32]
        lidt    [idtr32]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     SEL_KCODE32:pm_return

; do_rm_request: execute the rmcall block at [sys_req]
do_rm_request:
        mov     eax, [sys_req]
        mov     bx, ax
        and     bx, 0x000F
        shr     eax, 4
        mov     fs, ax                          ; FS:BX -> rmcall
        mov     [req_off], bx
        mov     al, [fs:bx+RC_INT]
        cmp     al, 0xFF
        je      nx_exit
        cmp     al, 0xFE
        je      nx_panic
        mov     [int_op+1], al
        mov     ax, [fs:bx+RC_AX]
        mov     cx, [fs:bx+RC_CX]
        mov     dx, [fs:bx+RC_DX]
        mov     si, [fs:bx+RC_SI]
        mov     di, [fs:bx+RC_DI]
        mov     bp, [fs:bx+RC_BP]
        push    word [fs:bx+RC_DS]
        push    word [fs:bx+RC_ES]
        mov     bx, [fs:bx+RC_BX]
        pop     es
        pop     ds
int_op: int     0x21                            ; patched
        pushf
        push    ds
        push    es
        push    bx
        mov     bx, cs
        mov     ds, bx
        mov     bx, [req_off]
        mov     [fs:bx+RC_AX], ax
        mov     [fs:bx+RC_CX], cx
        mov     [fs:bx+RC_DX], dx
        mov     [fs:bx+RC_SI], si
        mov     [fs:bx+RC_DI], di
        mov     [fs:bx+RC_BP], bp
        pop     ax
        mov     [fs:bx+RC_BX], ax
        pop     ax
        mov     [fs:bx+RC_ES], ax
        pop     ax
        mov     [fs:bx+RC_DS], ax
        pop     ax
        mov     [fs:bx+RC_FLAGS], ax
        mov     ax, cs
        mov     es, ax
        ret

; nx_panic: an exception happened in the program
nx_panic:
        call    set_text_mode
        mov     si, msg_nx_fault
        call    puts
        movzx   eax, byte [exc_vec]
        call    print_dec
        mov     si, msg_nx_eip
        call    puts
        mov     eax, [exc_eip]
        call    print_hex32
        mov     si, msg_nx_cs
        call    puts
        mov     eax, [exc_cs]
        call    print_hex32
        mov     si, msg_nx_err
        call    puts
        mov     eax, [exc_err]
        call    print_hex32
        mov     si, msg_nx_esp
        call    puts
        mov     eax, [exc_esp]
        call    print_hex32
        call    crlf
        ; the code bytes at EIP
        call    enter_unreal
        mov     esi, [exc_eip]
        mov     cx, 12
.byte:  movzx   eax, byte [gs:esi]
        call    print_hex16
        mov     al, ' '
        call    putc
        inc     esi
        loop    .byte
        call    crlf
        mov     si, msg_nx_fault
        call    log_puts
        movzx   eax, byte [exc_vec]
        call    log_dec
        mov     si, msg_nx_eip
        mov     eax, [exc_eip]
        call    log_line
        ; fall through
; nx_exit: the program is done (real mode, our stack)
nx_exit:
        cli
        call    kb_hw_reset                     ; empty the 8042 while IRQ1 is
        mov     al, [nx_pic1_mask]              ;  masked: the next keystroke
        out     0x21, al                        ;  then makes a fresh edge
        mov     al, [nx_pic2_mask]
        out     0xA1, al
        sti
        mov     byte [nx_running], 0
        call    snd_stream_stop_quiet
        mov     es, [nx_bounce_seg]
        call    mem_free
        mov     ax, cs
        mov     es, ax
        ; PIT back to 18.2 Hz, text mode back
        mov     al, 0x36
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        mov     ah, 0x0F
        int     0x10
        cmp     al, 0x03
        je      .mode_ok
        call    set_text_mode
.mode_ok:
        call    kb_flush
        mov     ss, [nx_ss]
        mov     sp, [nx_sp]
        call    log_flush                       ; what the program logged
        cmp     byte [nx_ret_type], 0
        je      .to_shell
        ; another program started it: clean up and return into its INT 21h
        ; frame, leaving the parent's memory and process context alone
        call    nx_cleanup
        cli
        mov     ss, [nx_ret_ss]
        mov     sp, [nx_ret_sp]
        sti
        mov     bp, sp
        and     R_FLAGS, 0xFFFE
        jmp     int21_direct
.to_shell:
        jmp     program_return                  ; RET lands in exec_program

; -----------------------------------------------------------------------------
; helpers
; -----------------------------------------------------------------------------
; pic_remap_pm: IRQ0-7 -> 20h-27h, IRQ8-15 -> 28h-2Fh, only IRQ0/1/2 unmasked
pic_remap_pm:
        mov     al, 0x11
        out     0x20, al
        out     0xA0, al
        mov     al, 0x20
        out     0x21, al
        mov     al, 0x28
        out     0xA1, al
        mov     al, 0x04
        out     0x21, al
        mov     al, 0x02
        out     0xA1, al
        mov     al, 0x01
        out     0x21, al
        out     0xA1, al
        mov     al, 0xF8                        ; IRQ0, IRQ1 and the cascade
        out     0x21, al
        mov     al, 0xEF                        ; IRQ12: the mouse
        out     0xA1, al
        ret

; pic_remap_rm: the BIOS mapping, IRQ0-7 -> 08h-0Fh, IRQ8-15 -> 70h-77h
pic_remap_rm:
        mov     al, 0x11
        out     0x20, al
        out     0xA0, al
        mov     al, 0x08
        out     0x21, al
        mov     al, 0x70
        out     0xA1, al
        mov     al, 0x04
        out     0x21, al
        mov     al, 0x02
        out     0xA1, al
        mov     al, 0x01
        out     0x21, al
        out     0xA1, al
        ret

; build_idt: 256 gates in idt32
build_idt:
        pusha
        push    es
        push    cs
        pop     es
        mov     di, idt32
        xor     cx, cx
.gate:  mov     ax, ignore_stub
        cmp     cx, 20
        jae     .not_exc
        mov     bx, cx
        shl     bx, 1
        mov     ax, [exc_table+bx]
        jmp     .store
.not_exc:
        cmp     cx, 0x20
        jne     .n1
        mov     ax, irq0_stub
        jmp     .store
.n1:    cmp     cx, 0x21
        jne     .n2
        mov     ax, irq1_stub
        jmp     .store
.n2:    cmp     cx, 0x22
        jb      .store
        cmp     cx, 0x27
        ja      .n3
        mov     ax, irq_master_stub
        jmp     .store
.n3:    cmp     cx, 0x2C                        ; IRQ12: the mouse
        jne     .n3b
        mov     ax, irq12_stub
        jmp     .store
.n3b:   cmp     cx, 0x28
        jb      .store
        cmp     cx, 0x2F
        ja      .n4
        mov     ax, irq_slave_stub
        jmp     .store
.n4:    cmp     cx, 0x80
        jne     .store
        mov     ax, syscall_stub
.store: stosw                                   ; offset 15:0
        mov     ax, SEL_KCODE32
        stosw
        mov     ax, 0x8E00                      ; present, 32-bit interrupt gate
        stosw
        xor     ax, ax
        stosw                                   ; offset 31:16 (kernel < 64 KB)
        inc     cx
        cmp     cx, NX_IDT_ENTRIES
        jb      .gate
        pop     es
        popa
        ret

exc_table:
        dw exc_0, exc_1, exc_2, exc_3, exc_4, exc_5, exc_6, exc_7, exc_8, exc_9
        dw exc_10, exc_11, exc_12, exc_13, exc_14, exc_15, exc_16, exc_17, exc_18, exc_19

; ext_mem_end: EAX = physical end of usable extended memory
ext_mem_end:
        push    ebx
        push    ecx
        push    edx
        mov     ax, 0xE801
        int     0x15
        jc      .try_88
        or      ax, ax
        jnz     .use_ax
        mov     ax, cx
        mov     bx, dx
.use_ax:
        movzx   eax, ax                         ; KB between 1 and 16 MB
        shl     eax, 10
        add     eax, 0x100000
        cmp     eax, 0x1000000
        jb      .done
        movzx   ebx, bx                         ; 64 KB blocks above 16 MB
        shl     ebx, 16
        add     eax, ebx
        jmp     .done
.try_88:
        mov     ah, 0x88
        int     0x15
        jc      .none
        movzx   eax, ax
        shl     eax, 10
        add     eax, 0x100000
        jmp     .done
.none:  mov     eax, 0x100000
.done:  cmp     eax, 0x8000000                  ; cap at 128 MB (plenty for DOS games)
        jbe     .capped
        mov     eax, 0x8000000
.capped:
        pop     edx
        pop     ecx
        pop     ebx
        ret

; -----------------------------------------------------------------------------
section .data
                align 8
gdt32:  dq 0
        dw 0xFFFF, KBASE & 0xFFFF               ; KCODE32: base KBASE, 4 GB
        db KBASE >> 16, 0x9A, 0xCF, 0x00
        dw 0xFFFF, 0x0000                       ; KDATA32: flat
        db 0x00, 0x92, 0xCF, 0x00
        dw 0xFFFF, KBASE & 0xFFFF               ; KCODE16: base KBASE, 64 KB
        db KBASE >> 16, 0x9A, 0x00, 0x00
        dw 0xFFFF, KBASE & 0xFFFF               ; KDATA16: base KBASE, 64 KB
        db KBASE >> 16, 0x92, 0x00, 0x00
        dw 0xFFFF, 0x0000                       ; ACODE32: flat code
        db 0x00, 0x9A, 0xCF, 0x00
gdt32_end:
gdtr32: dw gdt32_end - gdt32 - 1
        dd 0
idtr32: dw NX_IDT_ENTRIES * 8 - 1
        dd 0
idtr_real:
        dw 0x03FF
        dd 0
nx_running:     db 0
nx_pic1_mask:   db 0
nx_pic2_mask:   db 0
exc_vec:        db 0
                align 4
exc_eip:        dd 0
nx_ss:          dw 0
nx_sp:          dw 0
nx_bounce_seg:  dw 0
req_off:        dw 0
nx_load:        dd 0
nx_image_size:  dd 0
nx_remaining:   dd 0
nx_chunk:       dd 0
nx_mem_start:   dd 0
nx_mem_end:     dd 0
nx_stack_top:   dd 0
sys_req:        dd 0
pm_esp:         dd 0
far_ptr:        dd 0
                dw 0
panic_req:      times 18 db 0
                db 0xFE, 0
                dw 0
msg_nx_fault:   db "Program fault: exception ", 0
msg_nx_eip:     db " at EIP=", 0
msg_nx_cs:      db " CS=", 0
msg_nx_err:     db " ERR=", 0
msg_nx_esp:     db " ESP=", 0
                align 4
exc_err:        dd 0
exc_cs:         dd 0
exc_esp:        dd 0
section .bss
                alignb 16
nx_hdr:         resb 32
app_info:       resb AI_SIZE
idt32:          resb NX_IDT_ENTRIES * 8
nx_rm_stack:    resb 4096
nx_rm_stack_top:
section .text
