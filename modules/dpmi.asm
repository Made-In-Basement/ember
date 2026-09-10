; =============================================================================
;  DPMI.MOD - a DPMI 0.9 host for 32-bit clients (DOS/4GW and its kind)
; -----------------------------------------------------------------------------
;  Not loaded by default.  A DOS extender that finds a DPMI host prefers it
;  to running the processor itself, so this must be complete enough for the
;  program before it is advertised: LOAD DPMI when you want it.
;
;  What it is:
;   - The client runs in protected mode at ring 3, in a flat address space
;     without paging.  Its descriptors live in an LDT of 1024 entries.
;   - The interrupt controller stays where the BIOS put it (08h and 70h).
;     Vectors 8 to 15 are told apart from the exceptions that share them by
;     asking the controller which request is in service.
;   - Anything the client has not hooked in protected mode - DOS calls, the
;     timer, the keyboard - is reflected to real mode: the processor is
;     switched back, the real-mode handler runs on the real-mode stack, and
;     the processor is switched forward again.  Real-mode code reaches the
;     client through callbacks that make the same journey the other way.
;   - Memory for the client is the extended memory below the XMS pool, from
;     just above the high memory area up to where XMS.MOD's pool begins
;     (it is asked).  The kernel keeps that region for the 32-bit programs
;     it loads, and none is resident while a DOS program runs.  It must be
;     low: DOS/4GW keeps its tables where a 24-bit address can reach, so a
;     block above 16 MB makes it give up.  The host's own stacks and the
;     LDT are at the front of the region.
;   - CLI and STI from ring 3 trap; the host applies them to the client's
;     flags, so the client controls the real interrupt flag.  Port I/O is
;     allowed through (the permission bitmap is clear) until something wants
;     to watch a port.
;
;  This file is the real-mode side: loading, the INT 2Fh answer, the entry
;  point the client far-calls, the switch to protected mode, the executor
;  that runs real-mode interrupts on the client's behalf, and the real-mode
;  stubs of the callbacks.  dpmi_pm.inc is the 32-bit side; dpmi_31.inc the
;  INT 31h services.
; =============================================================================

[BITS 16]
%define MOD_ORG 0x1000
[ORG MOD_ORG]
%include "ember.inc"
%include "oplhisyms.inc"

; -----------------------------------------------------------------------------
;  Calling the synthesiser
; -----------------------------------------------------------------------------
;  opl3.c is C, and a C compiler takes it for granted that a pointer means the
;  same thing whether it came from a local or a global: one flat address
;  space, one base for the stack and the data both.  opl3_render proves it -
;  it declares two numbers, hands their addresses down, and the function below
;  writes the samples back through them.
;
;  In the virtual-8086 monitor that assumption holds, because the monitor's
;  ring-0 stack is inside the module and its data selector is based there too.
;  In the DPMI host it does not: the host's ring-0 stack is a flat address in
;  extended memory, so a pointer to a local, written through the data
;  selector, lands somewhere else entirely.  The chip then renders into
;  nothing and reads back whatever was at that address - which does not move,
;  so the music comes out as a number that barely changes.  That is what
;  silence with a small offset in it turned out to be.
;
;  So the synthesiser is called on a stack of the module's own, where the two
;  bases agree.  Nothing else in the host needs this, and the monitor, whose
;  stack already agrees, gets a pair of macros that do nothing.
%macro OPL_ENTER 0
        mov     [opl_ss], ss
        mov     [opl_esp], esp
        push    ds
        pop     ss                              ; the stack, based where the
        mov     esp, opl_stack_top              ;  data is
%endmacro
%macro OPL_LEAVE 0
        mov     ss, [opl_ss]
        mov     esp, [opl_esp]
%endmacro


        MODULE_HEADER "DPMI    ", dpmi_init, dpmi_unload, dpmi_event, 0

; ---- layout of the XMS block the client's world lives in ----
XA_LDT          equ 0x0000                      ; 1024 descriptors
XA_LDT_SIZE     equ 0x2000
XA_R0STACK      equ 0x2000                      ; the host's ring-0 stack
XA_R0STACK_TOP  equ 0x6000
XA_LOCKED       equ 0x6000                      ; the locked ring-3 stack
XA_LOCKED_TOP   equ 0xE000
XA_POOL         equ 0x10000                     ; the client's memory starts here
XA_MIN_KB       equ 512                         ; refuse a client with less

LDT_ENTRIES     equ 1024
SEL_INC         equ 8

; ---- GDT selectors ----
SEL_HCODE32     equ 0x08                        ; host code, base = this segment
SEL_HDATA32     equ 0x10                        ; host data, base = this segment, 4 GB
SEL_FLAT        equ 0x18                        ; everything, base 0
SEL_HCODE16     equ 0x20                        ; 16-bit host code (the way back)
SEL_HDATA16     equ 0x28                        ; 16-bit host data
SEL_TSS         equ 0x30
SEL_UCODE32     equ 0x3B                        ; ring-3 code for the return stubs
SEL_LDT         equ 0x40
GDT_ENTRIES     equ 9

; ---- the gate the ring-3 stubs come back through ----
RET_VECTOR      equ 0xF9
RAW_VECTOR      equ 0xF7                        ; a raw switch to real mode

; ---- real-mode call structure, as the specification lays it out ----
RC_EDI          equ 0
RC_ESI          equ 4
RC_EBP          equ 8
RC_EBX          equ 16
RC_EDX          equ 20
RC_ECX          equ 24
RC_EAX          equ 28
RC_FLAGS        equ 32
RC_ES           equ 34
RC_DS           equ 36
RC_FS           equ 38
RC_GS           equ 40
RC_IP           equ 42
RC_CS           equ 44
RC_SP           equ 46
RC_SS           equ 48
RC_SIZE         equ 50

; A scratch page below the excursion ring, for finding out whether a client
; ever touches the card at all: three counts and then a ring of what was on
; which port.  Read with tools/drtrace.py.
; %define DRLOG 1                        ; the card's traffic, into a page of
                                        ;  low memory - see tools/drtrace.py                        ; the card's traffic, into a page of
                                        ;  low memory - see tools/drtrace.py
; The synthesiser is C, and C assumes the stack is reached the same way as
; everything else.  This host's ring-0 stack is not: it is flat, while the
; host's own data is based at the module.  See OPL_ENTER in dpmi_io.inc.
%define OPL_OWN_STACK 1
DRLOG_LIN       equ 0x7800                      ; traps seen, ours, undecodable
DRRING_LIN      equ 0x7820                      ; port, value, direction
DRRING_MAX      equ 128                         ; and it wraps: the last 128
DRAUX_LIN       equ 0x7A40                      ; past the ring: what the card
                                                ;  and the pump think they are
                                                ;  doing, as of the last pump

TRACE_SEG       equ 0x07C0                      ; a page of real-mode excursions
; Six counters in the twenty-eight bytes between the excursion ring (which
; ends at 7DE4h) and the exception ring (which starts at 7E00h).  They were
; at 7DF0h, which ran into the exception ring's own count.
IRQCOUNT_LIN    equ 0x7DE4                      ; hardware interrupts: seen,
                                                ;  given to the client, sent
                                                ;  down to real mode
EXCTRACE_LIN    equ 0x7E00                      ; ...and, past them, the
EXCTRACE_MAX    equ 30                          ;  exceptions a client took
EXCTRACE_LIN    equ 0x7E00                      ; ...and, in its second half,
EXCTRACE_MAX    equ 30                          ;  the exceptions a client took
TRACE_LIN       equ 0x7C00
TRACE_ENTRIES   equ 40
ERRLOG_LIN      equ 0x7E00                      ; failed INT 31h calls
CALLTRACE_OFF   equ 0xE000                      ; the last INT 31h calls, a ring of
CALLTRACE_MAX   equ 500                         ;  16-byte entries in the XMS block
CALLBACKS       equ 16
CB_STUB_SIZE    equ 8                           ; bytes per real-mode callback stub
NEST_MAX        equ 16                          ; excursions in progress at once

; ---- sizes the tables below are built from ----
IOPB_BYTES      equ 128                         ; ports 0..3FFh; beyond them always trap
TSS_IOPB        equ 104
TSS_SIZE        equ 104 + IOPB_BYTES + 1
CB_ENTRY        equ 16                          ; cb_table: proc off (4), sel (2),
CB_PROC         equ 0                           ;  rmcs off (4), rmcs sel (2),
CB_PROC_SEL     equ 4                           ;  stack selector (2), pad
CB_RMCS         equ 6
CB_RMCS_SEL     equ 10
CB_STACK_SEL    equ 12
SEG_CACHE       equ 32
DOSMEM_MAX      equ 32
MEM_BLOCKS      equ 128
MB_BASE         equ 0
MB_SIZE_        equ 4
MB_STATE        equ 8                           ; 0 spare, 1 free, 2 in use
MB_SIZE         equ 12
VEC_STUB_SIZE   equ 12
DEF_VECTOR      equ 0xF8                        ; the gate behind the default handlers
DEF_STUB_SIZE   equ 8

; =============================================================================
; dpmi_init: needs XMS; hooks INT 2Fh to answer AX=1687h
; =============================================================================
dpmi_init:
        SVC_TABLE_COPY
        ; LOAD DPMI TRAP: run the client where its ports can be watched,
        ; at the price of the interrupt flag.  See client_iopl.
        call    wants_trapping
        jc      .iopl_kept
        mov     dword [client_iopl], 0
.iopl_kept:
        mov     ax, 0x4300
        int     0x2F
        cmp     al, 0x80
        jne     .no_xms
        mov     ax, 0x4310
        int     0x2F
        mov     [xms_entry], bx
        mov     [xms_entry+2], es
        ; this segment's physical address, for the descriptors
        mov     ax, cs
        movzx   eax, ax
        shl     eax, 4
        mov     [host_base], eax
        mov     [rm_entry_seg], cs
        call    build_gdt
        call    build_idt
        call    build_tss
        call    build_cb_stubs
        ; ---- INT 2Fh: ours first ----
        push    es
        xor     ax, ax
        mov     es, ax
        cli
        mov     eax, [es:0x2F*4]
        mov     [old_int2f], eax
        mov     word [es:0x2F*4], int2f_hook
        mov     [es:0x2F*4+2], cs
        sti
        pop     es
        mov     si, msg_loaded
        SVC     SVC_PUTS
        cmp     dword [client_iopl], 0
        jne     .said
        mov     si, msg_trapping
        SVC     SVC_PUTS
.said:  clc
        retf
.no_xms:
        mov     si, msg_no_xms
        SVC     SVC_PUTS
        stc
        retf

; wants_trapping: CF=0 if the load line said TRAP.  ES:SI is what followed
;   the module's name, spaces and all.
wants_trapping:
        push    ax
        push    si
.skip:  mov     al, [es:si]
        cmp     al, ' '
        jne     .word
        inc     si
        jmp     .skip
.word:  mov     al, [es:si]
        and     al, 0xDF                        ; upper case, roughly
        cmp     al, 'T'
        jne     .no
        mov     al, [es:si+1]
        and     al, 0xDF
        cmp     al, 'R'
        jne     .no
        mov     al, [es:si+2]
        and     al, 0xDF
        cmp     al, 'A'
        jne     .no
        mov     al, [es:si+3]
        and     al, 0xDF
        cmp     al, 'P'
        jne     .no
        pop     si
        pop     ax
        clc
        ret
.no:    pop     si
        pop     ax
        stc
        ret

; =============================================================================
; dpmi_unload: INT 2Fh back, if it is still ours and no client is running
; =============================================================================
dpmi_unload:
        cmp     byte [client_active], 0
        jne     .busy
        push    es
        xor     ax, ax
        mov     es, ax
        cmp     word [es:0x2F*4], int2f_hook
        jne     .hooked
        mov     ax, cs
        cmp     [es:0x2F*4+2], ax
        jne     .hooked
        cli
        mov     eax, [old_int2f]
        mov     [es:0x2F*4], eax
        sti
        pop     es
        clc
        retf
.hooked:
        pop     es
.busy:  stc
        retf

; =============================================================================
; dpmi_event: a program has ended - whatever client there was is gone
; =============================================================================
dpmi_event:
        cmp     al, MOD_EV_END
        je      .reset
        cmp     al, MOD_EV_PROMPT
        jne     .done
.reset: mov     byte [client_active], 0
.done:  retf

; =============================================================================
; int2f_hook: AX=1687h, the installation check
; =============================================================================
;   Out: AX = 0 (a host is here), BX bit 0 = 32-bit clients, CL = 4 (486),
;        DH.DL = 0.90, SI = paragraphs of data wanted, ES:DI = entry point.
;   The kernel's own handler is called first: it stands the speaker bridge
;   down, because its I/O breakpoints would fire in protected mode.
int2f_hook:
        cmp     ax, 0x1687
        jne     .chain
        cmp     byte [cs:client_active], 0      ; one client at a time
        jne     .chain                          ; (the kernel says: no host)
        pushf
        call    far [cs:old_int2f]
        xor     ax, ax
        mov     bx, 0x0001
        mov     cl, 0x04
        mov     dx, 0x005A
        mov     si, 1
        push    cs
        pop     es
        mov     di, dpmi_entry
        iret
.chain: jmp     far [cs:old_int2f]

; =============================================================================
; dpmi_entry: the client far-calls this in real mode with AX bit 0 set for a
;   32-bit client and ES = the paragraphs it allocated for us (unused).
;   Returns in protected mode with CF=0, or in real mode with CF=1.
; =============================================================================
dpmi_entry:
        test    al, 1
        jz      .refuse                         ; 16-bit clients: not here
        cmp     byte [cs:client_active], 0
        jne     .refuse
        ; ---- keep the client's registers ----
        mov     [cs:ent_eax], eax
        mov     [cs:ent_ebx], ebx
        mov     [cs:ent_ecx], ecx
        mov     [cs:ent_edx], edx
        mov     [cs:ent_esi], esi
        mov     [cs:ent_edi], edi
        mov     [cs:ent_ebp], ebp
        mov     [cs:ent_ds], ds
        mov     [cs:ent_ss], ss
        mov     [cs:ent_sp], sp
        pushf
        pop     word [cs:ent_flags]
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        ; ---- the client's PSP ----
        mov     ah, 0x62
        int     0x21
        mov     [client_psp], bx
        ; ---- extended memory for the client's world ----
        call    xms_take                        ; CF=1: none
        jc      .refuse_restore
        ; ---- per-client state ----
        call    client_reset
        push    es
        mov     ax, TRACE_SEG
        mov     es, ax
        mov     dword [es:0], 0
        mov     dword [es:ERRLOG_LIN - TRACE_LIN], 0
        pop     es
        ; ---- descriptors for the client's real-mode segments ----
        ; the LDT is in extended memory: write it through the block move
        mov     bx, sp
        ; the far call left IP, CS on the stack: the client resumes there
        mov     dx, [ss:bx]                     ; IP
        mov     [ent_ip], dx
        mov     dx, [ss:bx+2]                   ; CS
        mov     [ent_cs], dx
        add     bx, 4
        mov     [ent_sp], bx                    ; SP with the return address gone
        ; ---- go ----
        mov     byte [client_active], 1
        mov     byte [nest_depth], 0
        call    io_audio_open           ; the stream, for as long as it runs
        cli
        lgdt    [gdtr]
        lidt    [idtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     SEL_HCODE32:pm_first_entry
.refuse_restore:
        mov     eax, [ent_eax]
        mov     ds, [ent_ds]
.refuse:
        stc
        retf

; =============================================================================
; xms_take: the client's region, [xarea] and [xarea_kb]: from LOW_BASE to
;   where the XMS pool begins.  CF=1 if that is too little.
; =============================================================================
LOW_BASE        equ 0x110000                    ; above the high memory area
xms_take:
        mov     ah, 0x80                        ; ours: where is the pool?
        call    far [xms_entry]
        cmp     ax, 1
        jne     .none
        movzx   eax, dx
        shl     eax, 16
        mov     ax, bx                          ; EAX = the pool's base
        sub     eax, LOW_BASE
        jbe     .none
        shr     eax, 10                         ; KB
        cmp     eax, XA_MIN_KB
        jb      .none
        mov     [xarea_kb], eax
        mov     dword [xarea], LOW_BASE
        clc
        ret
.none:  stc
        ret

; xms_take_block: the old way, the largest XMS block, locked (unused)
xms_take_block:
        mov     ah, 0x08                        ; how much is free
        xor     bl, bl
        call    far [xms_entry]
        cmp     ax, XA_MIN_KB
        jb      .none
        mov     dx, ax
        mov     ah, 0x09                        ; allocate that
        call    far [xms_entry]
        or      ax, ax
        jz      .none
        mov     [xms_handle], dx
        mov     ah, 0x0C                        ; lock: DX:BX = the address
        call    far [xms_entry]
        or      ax, ax
        jz      .free
        mov     [xarea+2], dx
        mov     [xarea], bx
        mov     ah, 0x0E                        ; the size it really has
        mov     dx, [xms_handle]
        call    far [xms_entry]
        movzx   eax, dx
        mov     [xarea_kb], eax
        clc
        ret
.free:  mov     ah, 0x0A
        mov     dx, [xms_handle]
        call    far [xms_entry]
.none:  stc
        ret

; psp_env_restore: the environment paragraph back into the PSP
psp_env_restore:
        push    es
        mov     ax, [client_psp]
        or      ax, ax
        jz      .done
        mov     es, ax
        mov     ax, [es:0x2C]                   ; (noted, for the trace)
        push    es
        mov     bx, TRACE_SEG
        mov     es, bx
        mov     [es:0x2A0], ax
        mov     ax, [psp_env_seg]
        mov     [es:0x2A2], ax
        mov     ax, [client_psp]
        mov     [es:0x2A4], ax
        pop     es
        mov     ax, [psp_env_seg]
        mov     [es:0x2C], ax
.done:  pop     es
        ret

; xms_give: nothing to give back for the low region; a block, if one was taken
xms_give:
        cmp     word [xms_handle], 0
        je      .done
        mov     ah, 0x0D
        mov     dx, [xms_handle]
        call    far [xms_entry]
        mov     ah, 0x0A
        mov     dx, [xms_handle]
        call    far [xms_entry]
        mov     word [xms_handle], 0
.done:  ret

; =============================================================================
; client_reset: fresh tables for a new client (real mode, DS = this segment)
; =============================================================================
client_reset:
        push    es
        push    cs
        pop     es
        cld
        ; the LDT descriptor's base and the TSS's ring-0 stack
        mov     eax, [xarea]
        add     eax, XA_LDT
        mov     [gdt + SEL_LDT + 2], ax
        shr     eax, 16
        mov     [gdt + SEL_LDT + 4], al
        mov     [gdt + SEL_LDT + 7], ah
        mov     eax, [xarea]
        add     eax, XA_R0STACK_TOP
        mov     [tss + 4], eax                  ; ESP0
        mov     [r0_top], eax
        mov     eax, [xarea]
        add     eax, XA_LOCKED_TOP
        mov     [locked_sp], eax
        ; the allocation bitmap of the LDT, the vectors, the handles
        mov     di, ldt_used
        mov     cx, LDT_ENTRIES / 8
        xor     al, al
        rep     stosb
        mov     di, pm_vectors
        mov     cx, 256 * 8 / 2
        xor     ax, ax
        rep     stosw
        mov     di, exc_handlers
        mov     cx, 32 * 8 / 2
        rep     stosw
        mov     di, cb_used
        mov     cx, CALLBACKS
        rep     stosb
        mov     di, seg_cache
        mov     cx, SEG_CACHE * 2
        rep     stosw
        mov     di, dosmem_table
        mov     cx, DOSMEM_MAX * 2
        rep     stosw
        ; the memory pool: one free block
        mov     di, mem_blocks
        mov     cx, MEM_BLOCKS * MB_SIZE / 2
        rep     stosw
        mov     eax, [xarea]
        add     eax, XA_POOL
        mov     [mem_blocks + MB_BASE], eax
        mov     eax, [xarea_kb]
        shl     eax, 10
        sub     eax, XA_POOL
        mov     [mem_blocks + MB_SIZE_], eax
        mov     byte [mem_blocks + MB_STATE], 1
        mov     byte [ldt_next_hint], 0
        mov     byte [gdt + SEL_TSS + 5], 0x89   ; not busy: it will be loaded again
        mov     word [rm_stack_ptr], rm_stack_top
        mov     dword [exc_count], 0
        pop     es
        ret

; =============================================================================
; the descriptor tables (built once at load; bases patched at client entry)
; =============================================================================
build_gdt:
        push    es
        push    cs
        pop     es
        mov     di, gdt
        xor     eax, eax
        mov     cx, GDT_ENTRIES * 4
        rep     stosw
        ; HCODE32: base host, limit 4 GB, code, DPL 0, 32-bit
        mov     di, gdt + SEL_HCODE32
        mov     eax, [host_base]
        mov     bl, 0x9A
        mov     bh, 0xCF
        call    put_desc
        mov     di, gdt + SEL_HDATA32
        mov     bl, 0x92
        mov     bh, 0xCF
        call    put_desc
        mov     di, gdt + SEL_FLAT
        xor     eax, eax
        mov     bl, 0x92
        mov     bh, 0xCF
        call    put_desc
        mov     di, gdt + SEL_HCODE16
        mov     eax, [host_base]
        mov     bl, 0x9A
        mov     bh, 0x00
        call    put_desc
        mov     di, gdt + SEL_HDATA16
        mov     bl, 0x92
        mov     bh, 0x00
        call    put_desc
        mov     di, gdt + (SEL_UCODE32 & ~7)
        mov     bl, 0xFA                        ; code, DPL 3
        mov     bh, 0xCF
        call    put_desc
        ; TSS: base host + tss, limit = its size - 1
        mov     di, gdt + SEL_TSS
        mov     eax, [host_base]
        add     eax, tss
        mov     [di+2], ax
        shr     eax, 16
        mov     [di+4], al
        mov     [di+7], ah
        mov     word [di], TSS_SIZE - 1
        mov     byte [di+5], 0x89               ; available 32-bit TSS
        mov     byte [di+6], 0
        ; LDT: base filled in per client, limit = 1024 * 8 - 1
        mov     di, gdt + SEL_LDT
        mov     word [di], LDT_ENTRIES * 8 - 1
        mov     byte [di+5], 0x82
        mov     byte [di+6], 0
        mov     eax, [host_base]
        add     eax, gdt
        mov     [gdtr+2], eax
        mov     eax, [host_base]
        add     eax, idt
        mov     [idtr+2], eax
        pop     es
        ret

; put_desc: DI -> descriptor, EAX = base, BL = access, BH = flags (G/D + limit hi)
;   limit is FFFFF with G set when BH has bit 7, else FFFF
put_desc:
        push    eax
        mov     word [di], 0xFFFF
        mov     [di+2], ax
        shr     eax, 16
        mov     [di+4], al
        mov     [di+7], ah
        mov     [di+5], bl
        mov     al, bh
        and     al, 0xF0
        test    bh, 0x80
        jz      .small
        or      al, 0x0F
.small: mov     [di+6], al
        pop     eax
        ret

; build_idt: 256 interrupt gates, DPL 3, into the 32-bit stubs
build_idt:
        push    es
        push    cs
        pop     es
        mov     di, idt
        xor     cx, cx
.gate:  mov     ax, cx
        imul    ax, VEC_STUB_SIZE
        add     ax, vec_stubs
        stosw                                   ; offset 15:0
        mov     ax, SEL_HCODE32
        stosw
        mov     ax, 0xEE00                      ; present, DPL 3, 32-bit interrupt gate
        stosw
        xor     ax, ax
        stosw                                   ; offset 31:16
        inc     cx
        cmp     cx, 256
        jb      .gate
        pop     es
        ret

; build_tss: the ring-0 stack selector and an I/O map that lets everything through
build_tss:
        push    es
        push    cs
        pop     es
        mov     di, tss
        mov     cx, TSS_SIZE / 2
        xor     ax, ax
        rep     stosw
        mov     word [tss + 8], SEL_FLAT        ; SS0
        mov     word [tss + 102], TSS_IOPB      ; the bitmap's offset
        mov     byte [tss + TSS_IOPB + IOPB_BYTES], 0xFF    ; the closing byte
        pop     es
        call    io_trap_setup           ; ...except the card own ports
        ret

; build_cb_stubs: CALLBACKS little real-mode routines, "push n; jmp cb_common"
build_cb_stubs:
        push    es
        push    cs
        pop     es
        mov     di, cb_stubs
        xor     cx, cx
.stub:  mov     al, 0x68                        ; push imm16
        stosb
        mov     ax, cx
        stosw
        mov     al, 0xE9                        ; jmp rel16
        stosb
        mov     ax, cb_common
        sub     ax, di
        sub     ax, 2
        stosw
        mov     ax, 0x9090
        stosw                                   ; pad to CB_STUB_SIZE
        inc     cx
        cmp     cx, CALLBACKS
        jb      .stub
        pop     es
        ret

; =============================================================================
; rm_run: execute the request described by rm_req (real mode, DS = this
;   segment, interrupts off).  rm_kind says what: 0 = interrupt rm_vector,
;   1 = far call to rm_req CS:IP, 2 = far call with flags pushed (IRET frame).
;   The registers come from rm_req and go back there.  If rm_req SS:SP is 0
;   the host's real-mode stack is used, with rm_copy words copied from
;   rm_copy_src (a linear address) on top.
; =============================================================================
rm_run:
        push    word [rm_own_stack]             ; the level outside this one
        push    word [rm_saved_sp]
        ; ---- the stack ----
        mov     ax, [rm_req + RC_SS]
        or      ax, [rm_req + RC_SP]
        jnz     .their_stack
        mov     ax, cs
        mov     [rm_req + RC_SS], ax
        ; our stack: 256 bytes under the current top are ours, the 768
        ; below those the handler's; a nested excursion starts lower still
        mov     ax, [rm_stack_ptr]
        sub     ax, 0x100
        mov     [rm_req + RC_SP], ax
        sub     ax, 0x300
        mov     [rm_stack_ptr], ax
        mov     byte [rm_own_stack], 1
        jmp     .stack_set
.their_stack:
        mov     byte [rm_own_stack], 0
.stack_set:
        ; ---- copy CX words from the client's stack onto the real-mode one ----
        movzx   ecx, word [rm_copy]
        jecxz   .no_copy
        mov     ax, [rm_req + RC_SP]
        shl     cx, 1
        sub     ax, cx
        mov     [rm_req + RC_SP], ax
        shr     cx, 1
        push    ds
        push    es
        mov     es, [rm_req + RC_SS]
        mov     di, ax
        mov     esi, [rm_copy_src]
        mov     ax, si
        and     ax, 0x000F
        shr     esi, 4
        mov     ds, si
        mov     si, ax
        rep     movsw
        pop     es
        pop     ds
.no_copy:
        ; ---- what to run: patch the frame ----
        mov     [rm_saved_sp], sp
        mov     al, [rm_kind]
        or      al, al
        jnz     .not_int
        ; an interrupt: the vector's address
        push    es
        xor     ax, ax
        mov     es, ax
        movzx   bx, byte [rm_vector]
        shl     bx, 2
        mov     eax, [es:bx]
        pop     es
        mov     [rm_target], eax
        jmp     .flags_frame
.not_int:
        mov     ax, [rm_req + RC_IP]
        mov     [rm_target], ax
        mov     ax, [rm_req + RC_CS]
        mov     [rm_target+2], ax
        cmp     byte [rm_kind], 1
        je      .load
.flags_frame:
        ; push the flags on the target stack, as INT would, then far-call
        mov     ax, [rm_req + RC_SP]
        sub     ax, 2
        mov     [rm_req + RC_SP], ax
        push    ds
        mov     ds, [rm_req + RC_SS]
        mov     bx, ax
        mov     ax, [cs:rm_req + RC_FLAGS]
        and     ax, ~0x0100                     ; never single-step a handler
        mov     [bx], ax
        pop     ds
.load:  ; ---- note it: kind, vector, AX, BX ----
        push    es
        push    bx
        mov     ax, TRACE_SEG
        mov     es, ax
        mov     eax, [es:0]
        inc     dword [es:0]
        push    dx
        xor     dx, dx
        push    cx
        mov     cx, TRACE_ENTRIES
        div     cx                              ; a ring: DX = the slot
        pop     cx
        mov     ax, dx
        pop     dx
        imul    bx, ax, 12
        add     bx, 4
        mov     al, [rm_kind]
        mov     [es:bx], al
        mov     al, [rm_vector]
        mov     [es:bx+1], al
        mov     ax, [rm_req + RC_EAX]
        mov     [es:bx+2], ax
        mov     ax, [rm_req + RC_EBX]
        mov     [es:bx+4], ax
        mov     [trace_slot], bx
.noted: pop     bx
        pop     es
        ; ---- the registers, then the call ----
        mov     eax, [rm_req + RC_EAX]
        mov     ebx, [rm_req + RC_EBX]
        mov     ecx, [rm_req + RC_ECX]
        mov     edx, [rm_req + RC_EDX]
        mov     esi, [rm_req + RC_ESI]
        mov     edi, [rm_req + RC_EDI]
        mov     ebp, [rm_req + RC_EBP]
        mov     es, [rm_req + RC_ES]
        mov     fs, [rm_req + RC_FS]
        mov     gs, [rm_req + RC_GS]
        mov     ss, [rm_req + RC_SS]
        mov     sp, [rm_req + RC_SP]
        push    word [cs:rm_req + RC_FLAGS]
        popf                                    ; the client's IF for the handler
        mov     ds, [cs:rm_req + RC_DS]
        call    far [cs:rm_target]
        ; ---- back: everything into the request ----
        pushf
        pop     word [cs:rm_req + RC_FLAGS]
        cli
        mov     [cs:rm_req + RC_EAX], eax
        mov     [cs:rm_req + RC_EBX], ebx
        mov     [cs:rm_req + RC_ECX], ecx
        mov     [cs:rm_req + RC_EDX], edx
        mov     [cs:rm_req + RC_ESI], esi
        mov     [cs:rm_req + RC_EDI], edi
        mov     [cs:rm_req + RC_EBP], ebp
        mov     [cs:rm_req + RC_DS], ds
        mov     [cs:rm_req + RC_ES], es
        mov     [cs:rm_req + RC_FS], fs
        mov     [cs:rm_req + RC_GS], gs
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, [rm_saved_sp]
        ; ---- and how it went ----
        mov     ax, TRACE_SEG
        mov     es, ax
        mov     bx, [trace_slot]
        or      bx, bx
        jz      .not_noted
        mov     ax, [rm_req + RC_EAX]
        mov     [es:bx+6], ax
        mov     ax, [rm_req + RC_FLAGS]
        mov     [es:bx+8], ax
        mov     word [trace_slot], 0
.not_noted:
        mov     ax, cs
        mov     es, ax
        cmp     byte [rm_own_stack], 0
        je      .done
        add     word [rm_stack_ptr], 0x400      ; our part of the stack is free again
.done:  pop     word [rm_saved_sp]
        pop     word [rm_own_stack]
        ret

; =============================================================================
; cb_common: real-mode code called one of the callbacks (its number is on the
;   stack, above the far return address).  Take the registers, cross over.
; =============================================================================
cb_common:
        pushf
        cli
        mov     [cs:cb_regs + RC_EAX], eax
        pop     ax
        mov     [cs:cb_regs + RC_FLAGS], ax
        pop     ax                              ; the callback number
        mov     [cs:cb_index], ax
        mov     [cs:cb_regs + RC_EBX], ebx
        mov     [cs:cb_regs + RC_ECX], ecx
        mov     [cs:cb_regs + RC_EDX], edx
        mov     [cs:cb_regs + RC_ESI], esi
        mov     [cs:cb_regs + RC_EDI], edi
        mov     [cs:cb_regs + RC_EBP], ebp
        mov     [cs:cb_regs + RC_DS], ds
        mov     [cs:cb_regs + RC_ES], es
        mov     [cs:cb_regs + RC_FS], fs
        mov     [cs:cb_regs + RC_GS], gs
        mov     [cs:cb_regs + RC_SS], ss
        mov     [cs:cb_regs + RC_SP], sp        ; -> the far return address
        mov     ax, cs
        mov     ds, ax
        mov     byte [pm_entry_kind], 1         ; a callback
        jmp     enter_pm

; =============================================================================
; raw_rm_entry: real-mode code jumped here to switch itself to protected
;   mode the raw way (INT 31h 0306h): AX = DS, CX = ES, DX = SS, EBX = ESP,
;   SI = CS, EDI = EIP, all selectors.  Interrupts are off already.
; =============================================================================
raw_rm_entry:
        cli
        mov     [cs:cb_regs + RC_EAX], eax
        mov     [cs:cb_regs + RC_EBX], ebx
        mov     [cs:cb_regs + RC_ECX], ecx
        mov     [cs:cb_regs + RC_EDX], edx
        mov     [cs:cb_regs + RC_ESI], esi
        mov     [cs:cb_regs + RC_EDI], edi
        mov     [cs:cb_regs + RC_EBP], ebp
        mov     ax, cs
        mov     ds, ax
        mov     byte [pm_entry_kind], 2         ; a raw switch
        jmp     enter_pm

; =============================================================================
; enter_pm: from real mode into the 32-bit host code (interrupts are off).
;   pm_entry_kind says why: 0 = an excursion has finished, 1 = a callback.
;   The ring-0 stack continues where the last departure left it.
; =============================================================================
enter_pm:
        lgdt    [gdtr]
        lidt    [idtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     SEL_HCODE32:pm_reenter

; =============================================================================
; leave_pm: arrived from the 32-bit side (CS = SEL_HCODE16), go to real mode
;   and run what rm_kind asks, then return to protected mode
; =============================================================================
leave_pm:
        mov     ax, SEL_HDATA16
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     eax, cr0
        and     al, 0xFE
        mov     cr0, eax
        jmp     far [cs:rm_entry_ptr]           ; CS = this segment again
rm_entry:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, [rm_stack_ptr]
        lidt    [idtr_real]
        cmp     byte [rm_kind], 3
        je      rm_terminate
        cmp     byte [rm_kind], 4
        je      rm_fault
        cmp     byte [rm_kind], 5
        je      rm_resume
        cmp     byte [rm_kind], 6
        je      rm_desc_bad
        call    rm_run
        mov     byte [pm_entry_kind], 0
        jmp     enter_pm

; rm_resume: a callback's procedure is done; real mode goes on from rm_req
rm_resume:
        mov     eax, [rm_req + RC_EAX]
        mov     ebx, [rm_req + RC_EBX]
        mov     ecx, [rm_req + RC_ECX]
        mov     edx, [rm_req + RC_EDX]
        mov     esi, [rm_req + RC_ESI]
        mov     edi, [rm_req + RC_EDI]
        mov     ebp, [rm_req + RC_EBP]
        mov     es, [rm_req + RC_ES]
        mov     fs, [rm_req + RC_FS]
        mov     gs, [rm_req + RC_GS]
        mov     ss, [rm_req + RC_SS]
        mov     sp, [rm_req + RC_SP]
        push    word [cs:rm_req + RC_FLAGS]
        push    word [cs:rm_req + RC_CS]
        push    word [cs:rm_req + RC_IP]
        mov     ds, [cs:rm_req + RC_DS]
        iret

; rm_terminate: the client is ending (INT 21h 4Ch or the like): let go of
;   everything, then run the request for real.  It does not come back.
; io_watch_off: the breakpoints away again, before the kernel has the
;   machine back.  Real mode is privilege level nought, so this is allowed.
io_watch_off:
        push    eax
        xor     eax, eax
        mov     dr7, eax
        mov     dr6, eax
        pop     eax
        ret

rm_terminate:
        mov     byte [client_active], 0
        call    io_watch_off
        call    io_report
        call    io_audio_close                       ; what it asked the card for
        call    psp_env_restore
        call    xms_give
        mov     byte [rm_kind], 0
        mov     word [rm_req + RC_SS], 0        ; on our stack, whatever was asked
        mov     word [rm_req + RC_SP], 0
        mov     word [rm_copy], 0
        call    rm_run
        ; a terminate request that returned: end the program ourselves
        mov     ax, 0x4C00
        int     0x21
        jmp     $

; rm_desc_bad: a descriptor the host built does not read back as it was
;   asked for - a bug in this host, not in the client.  Say which and what,
;   and end the program before the client faults on it somewhere less
;   informative.
rm_desc_bad:
        mov     byte [client_active], 0
        call    psp_env_restore
        call    xms_give
        mov     si, msg_desc_bad
        SVC     SVC_PUTS
        mov     ax, [dc_sel]
        SVC     SVC_PRINT_HEX16
        mov     si, msg_desc_want
        SVC     SVC_PUTS
        mov     si, dc_base
        call    rm_print_hex32
        mov     si, msg_desc_limit
        SVC     SVC_PUTS
        mov     si, dc_limit
        call    rm_print_hex32
        mov     si, msg_desc_got
        SVC     SVC_PUTS
        mov     si, dc_got_base
        call    rm_print_hex32
        mov     si, msg_desc_limit
        SVC     SVC_PUTS
        mov     si, dc_got_limit
        call    rm_print_hex32
        SVC     SVC_CRLF
        mov     ax, 0x4CFF
        int     0x21
        jmp     $

; rm_print_hex32: SI -> a dword, print it (the services take and take back SI)
rm_print_hex32:
        push    si
        mov     ax, [si+2]
        SVC     SVC_PRINT_HEX16
        pop     si
        push    si
        mov     ax, [si]
        SVC     SVC_PRINT_HEX16
        pop     si
        ret

; rm_fault: an exception nobody handled.  Say where, and end the program.
rm_fault:
        mov     byte [client_active], 0
        call    io_watch_off
        call    io_report
        call    io_audio_close                       ; what it had asked the card for
        call    psp_env_restore
        call    xms_give
        mov     si, msg_fault
        SVC     SVC_PUTS
        movzx   eax, byte [fault_vec]
        SVC     SVC_PRINT_DEC
        mov     si, msg_fault_at
        SVC     SVC_PUTS
        mov     ax, [fault_cs]
        SVC     SVC_PRINT_HEX16
        mov     al, ':'
        SVC     SVC_PUTC
        mov     ax, [fault_eip+2]
        SVC     SVC_PRINT_HEX16
        mov     ax, [fault_eip]
        SVC     SVC_PRINT_HEX16
        mov     si, msg_fault_err
        SVC     SVC_PUTS
        mov     ax, [fault_err+2]
        SVC     SVC_PRINT_HEX16
        mov     ax, [fault_err]
        SVC     SVC_PRINT_HEX16
        SVC     SVC_CRLF
        mov     si, msg_fault
        mov     eax, [fault_eip]
        SVC     SVC_LOG_LINE
        mov     ax, 0x4CFF
        int     0x21
        jmp     $

%include "dpmi_pm.inc"
%include "dpmi_31.inc"
%include "dpmi_io.inc"

; =============================================================================
section .data
                align 8
gdt:            times GDT_ENTRIES * 8 db 0
gdtr:           dw GDT_ENTRIES * 8 - 1
                dd 0
idtr:           dw 256 * 8 - 1
                dd 0
idtr_real:      dw 0x03FF
                dd 0
rm_entry_ptr:   dw rm_entry
rm_entry_seg:   dw 0                            ; this segment, set at init
old_int2f:      dd 0
xms_entry:      dd 0
xms_handle:     dw 0
host_base:      dd 0
xarea:          dd 0
xarea_kb:       dd 0
                align 4
client_iopl:    dd 0x3000                       ; see the note in dpmi_pm.inc
client_active:  db 0
client_psp:     dw 0
nest_depth:     db 0
pm_entry_kind:  db 0
rm_kind:        db 0
rm_vector:      db 0
rm_own_stack:   db 0
ldt_next_hint:  db 0
rm_copy:        dw 0
rm_copy_src:    dd 0
rm_saved_sp:    dw 0
trace_slot:     dw 0
rm_stack_ptr:   dw 0
rm_target:      dd 0
cb_index:       dw 0
r0_top:         dd 0
locked_sp:      dd 0
exc_count:      dd 0
fault_vec:      db 0
                align 4
fault_eip:      dd 0
fault_cs:       dd 0
fault_err:      dd 0
ent_eax:        dd 0
ent_ebx:        dd 0
ent_ecx:        dd 0
ent_edx:        dd 0
ent_esi:        dd 0
ent_edi:        dd 0
ent_ebp:        dd 0
ent_ds:         dw 0
ent_ss:         dw 0
ent_sp:         dw 0
ent_ip:         dw 0
ent_cs:         dw 0
ent_flags:      dw 0
svc_table:      times SVC_MAX * 4 db 0
msg_loaded:     db "DPMI: host ready for 32-bit clients (DOS/4GW), 0.9", 13, 10, 0
msg_trapping:   db "DPMI: clients run where their ports can be watched, which "
                db "costs them the interrupt flag", 13, 10, 0
msg_no_xms:     db "DPMI: needs extended memory - LOAD XMS first", 13, 10, 0
msg_fault:      db "DPMI: unhandled exception ", 0
msg_fault_at:   db " at ", 0
msg_fault_err:  db " error ", 0
msg_desc_bad:   db "DPMI: descriptor ", 0
msg_desc_want:  db " was asked for base ", 0
msg_desc_limit: db " limit ", 0
msg_desc_got:   db ", reads back base ", 0
dc_sel:         dw 0
dc_base:        dd 0
dc_limit:       dd 0
dc_got_base:    dd 0
dc_got_limit:   dd 0

section .data
                align 16
tss:            times TSS_SIZE db 0
idt:            times 256 * 8 db 0
rm_req:         times RC_SIZE db 0              ; the request being run in real mode
                align 4
cb_regs:        times RC_SIZE db 0              ; what a callback was called with
cb_stubs:       times CALLBACKS * CB_STUB_SIZE db 0
cb_table:       times CALLBACKS * CB_ENTRY db 0 ; PM procedure, its structure
cb_used:        times CALLBACKS db 0
ldt_used:       times LDT_ENTRIES / 8 db 0      ; one bit per descriptor
pm_vectors:     times 256 * 8 db 0              ; offset (4), selector (2), pad
exc_handlers:   times 32 * 8 db 0
seg_cache:      times SEG_CACHE * 4 db 0        ; segment (2), selector (2)
dosmem_table:   times DOSMEM_MAX * 4 db 0       ; first selector (2), count (2)
mem_blocks:     times MEM_BLOCKS * MB_SIZE db 0
nest_esp:       times NEST_MAX dd 0             ; the ring-0 stack at each departure
rm_stack:       times 8192 db 0
rm_stack_top:
bss_end:

; =============================================================================
;  The synthesiser
; -----------------------------------------------------------------------------
;  The same nano/opl/opl3.c that SB.MOD carries, linked to sit where this
;  module's own code and data end rather than where that one's do - see the
;  two entries in tools/build_opl.py.  Every address in it is an offset in
;  this module, and this module's selectors are based on this module, so it
;  works wherever the kernel puts us.
;
;  A DPMI client's music comes through here for the same reason a real-mode
;  game's does: the ports it writes are watched, the registers are handed to
;  the chip, and io_pump asks the chip for samples along with everything else
;  that is making a sound.
; =============================================================================
%ifdef HAVE_OPL
section .opl start=OPL_ORG
opl_image:      incbin "oplhi.bin"
                times (OPL_END - OPL_ORG) - ($ - $$) db 0
opl_end:
%endif

section .text
