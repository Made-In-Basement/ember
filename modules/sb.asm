; =============================================================================
;  SB.MOD - a Sound Blaster for ordinary DOS programs
; -----------------------------------------------------------------------------
;  The card in dpmi_io.inc works because the program using it runs under a
;  task state segment whose permission map refuses the ports a Sound Blaster
;  would answer to.  The processor raises a general protection fault *before*
;  the instruction runs, so a read can be given a value no chip supplied, and
;  the host answers as the card would.
;
;  That only helped programs that had gone into protected mode themselves and
;  asked a DPMI host for a world to live in - which is almost no games at all.
;  A game from the era this machine is pretending to be from runs in real
;  mode, owns the whole processor, and there is no permission map anywhere
;  near it.
;
;  So this module puts it in virtual-8086 mode instead.  The program still
;  sees a megabyte of memory addressed as segment and offset, still has the
;  interrupt vector table at zero, still calls DOS through INT 21h - nothing
;  it can see has changed.  But it is now running at ring 3 under a task
;  state segment, and the same permission map that made the DPMI card work
;  makes this one work.  Nothing else about the machine moves: no paging, no
;  memory remapped, no extender, no descriptors for the program to know
;  about.  It is the smallest thing that turns a fault into a sound card.
;
;  What runs in virtual-8086 mode is not only the game: the kernel's own
;  INT 21h, the BIOS, the interrupt handlers, everything from the moment a
;  program is about to start until it has ended.  That is safe because none
;  of it needs anything virtual-8086 mode takes away - the kernel reaches
;  memory above a megabyte through a 4 GB GS only in the sound driver, and
;  the sound driver does not run while a program does.
;
;  Where the two modes meet:
;    v86_enter   from real mode; comes back at the caller's next instruction,
;                in virtual-8086 mode, with everything else unchanged.
;    v86_leave   the same journey the other way, asked for by writing to a
;                port that does not exist.
;  Between those two calls the machine is doing exactly what it was doing
;  before, one privilege level down.
;
;  What reaches this monitor:
;    - a general protection fault from a watched port, which the card answers
;    - a hardware interrupt, which is handed to the program's own handler
;    - and on the timer, the pump that walks the game's buffer into the
;      machine's own audio stream, and the card's interrupt when a block of
;      it has been played
;  Everything else - every INT the program makes - the processor sends
;  straight to the real-mode vector without stopping here, using the
;  redirection bitmap when the processor has one and this monitor's own
;  reflection when it has not.
; =============================================================================

[BITS 16]
; The high memory area is 64 KB and this module is now most of a synthesiser
; as well as a monitor, so it takes the slot DPMI.MOD uses.  They are never
; both wanted: one is for programs that go into protected mode themselves and
; the other for programs that never heard of it.  Whichever is loaded first
; has the high memory area and the other goes into conventional memory.
%define MOD_ORG 0x1000
[ORG MOD_ORG]
%include "ember.inc"
%include "oplsyms.inc"

        MODULE_HEADER "SB      ", sb_init, sb_unload, sb_event, 0

; ---- GDT selectors ----------------------------------------------------------
SEL_CODE32      equ 0x08                ; monitor code, base = this segment
SEL_DATA32      equ 0x10                ; monitor data, base = this segment
SEL_FLAT        equ 0x18                ; everything, base 0
SEL_CODE16      equ 0x20                ; the way back to real mode
SEL_DATA16      equ 0x28
SEL_TSS         equ 0x30
GDT_ENTRIES     equ 7

; ---- the task state segment -------------------------------------------------
IOPB_BYTES      equ 128                 ; ports 0..3FFh; beyond them, refused
TSS_IOPB        equ 104
TSS_SIZE        equ TSS_IOPB + IOPB_BYTES + 1

; ---- where the interrupt controller is put while the monitor is up ----------
;  The controller the BIOS leaves behind puts the eight hardware requests on
;  vectors 8 to 15, which are also where the processor's own exceptions are.
;  A DPMI host has to ask the controller which of the two it is looking at
;  every time.  Here there is no need to be clever: the controller is moved
;  out of the way for as long as the monitor is up, and moved back after.
PIC_M_BASE      equ 0x20
PIC_S_BASE      equ 0x28

; ---- what feeds the sound ---------------------------------------------------
;  The pump has to run often enough that the audio ring never empties and,
;  now that there is a synthesiser behind it, often enough that music is made
;  close to the moment it is played: anything rendered far ahead of time is
;  rendered from registers the game has not written yet, and a tune comes out
;  in lumps.
;
;  The obvious clock is the timer, and the obvious trick is to run it faster
;  than the game asked and pass the game every Nth tick.  That works, and
;  every monitor of this kind does it, but it means lying to a game about the
;  one clock it uses for everything - and a game that reads the counter
;  rather than counting interrupts gets a wrong answer.
;
;  So this uses the other clock instead.  The real-time chip has a periodic
;  interrupt of its own on the second controller's first line, nothing in a
;  DOS game has ever used it, and it is nobody's timebase.  At 256 a second
;  the pump runs every four milliseconds and the game's timer is left exactly
;  as the game set it.
RTC_RATE        equ 8                   ; 32768 >> (rate - 1) = 256 a second
RTC_IRQ         equ 8                   ; as this monitor numbers them
RTC_IRQ_SLAVE   equ 0                   ; ...and as the second controller does

; ---- the frame a fault leaves, once a stub has pushed everything ------------
;  Deliberately the same shape as the DPMI host's, so io_decode.inc reads
;  both.  The four words below EDI are padding there; here they are padding
;  too - virtual-8086 mode puts the program's segments above SS instead.
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
F_ES            equ 76
F_DS            equ 80
F_FS            equ 84
F_GS            equ 88

EFL_IF          equ 0x00000200
EFL_TF          equ 0x00000100
EFL_AC          equ 0x00040000
EFL_VM          equ 0x00020000
EFL_IOPL3       equ 0x00003000
; The program runs with an I/O privilege level of nought, which is what makes
; all of this unambiguous: CLI, STI, PUSHF, POPF, INT and IRET all fault into
; the monitor rather than executing, so nothing a program does can arrive on
; the vectors the interrupt controller has been moved to.  It costs a fault
; per DOS call and per interrupt, which on this processor is nothing at all.
V86_FLAGS       equ EFL_VM | EFL_IF | 2
EFL_OF          equ 0x00000800
EFL_NT          equ 0x00004000
; The bits a program may set for itself, and the whole point of the list is
; that it be right: carry (0), parity (2), adjust (4), zero (6), sign (7),
; trap (8), interrupt (9), direction (10) and overflow (11).  With bit 9
; missing out of this, every POPF and every IRET switched interrupts off
; again, so the first one to arrive was the last: the clock a program keeps
; time by stopped, and the program with it.
EFL_PROGRAM     equ 0x00000FD5
; And the ones it may believe it has set without them being true.  A program
; finds out it is on a 386 rather than a 286 by setting the privilege level
; and nested-task bits, reading them back, and seeing whether they stuck: on
; a 286 they never do.  They must not really stick here - the monitor needs
; the privilege level at nought for anything to trap at all - so they are
; remembered instead and handed back when the program asks.  Without that,
; DOS/4GW decides the processor is a 286 and refuses to run.
EFL_VIRTUAL     equ 0x00007000                  ; the two IOPL bits and NT

VEC_STUB_SIZE   equ 12
%define IO_DEFAULT32 0                  ; an 8086 program's words are 16 bits

; =============================================================================
;  Loading
; =============================================================================
sb_init:
        SVC_TABLE_COPY
        mov     ax, cs                          ; where we are, linearly
        movzx   eax, ax
        shl     eax, 4
        mov     [host_base], eax
        mov     [rm_seg], cs
        mov     [rm_fault_seg], cs
        call    build_gdt
        call    build_idt
        call    build_tss
        mov     si, msg_loaded
        SVC     SVC_PUTS
        clc
        retf

sb_unload:
        cmp     byte [in_v86], 0
        jne     .busy
        clc
        retf
.busy:  stc
        retf

; =============================================================================
; sb_event: a program is about to run, or has just ended.  Those are the two
;   moments the machine changes mode.  Both are far calls from the kernel, so
;   both come back to the kernel - one in virtual-8086 mode, one out of it.
; =============================================================================
sb_event:
        cmp     al, MOD_EV_START
        je      .start
        cmp     al, MOD_EV_END
        je      .end
        cmp     al, MOD_EV_NATIVE               ; a 32-bit program of our own:
        je      .end                            ;  it runs the processor itself
        retf
.start:
        cmp     byte [in_v86], 0
        jne     .out                            ; already, somehow
        call    io_card_reset
        call    io_audio_open                   ; the stream, while it runs
        call    v86_enter                       ; ...and back here, in V86
        retf
.end:
        cmp     byte [in_v86], 0
        je      .quiet
        call    v86_leave                       ; ...and back here, out of it
        cmp     dword [card_irqs], 0
        je      .quiet
        mov     si, msg_irqs
        mov     eax, [card_irqs]
        call    io_say
.quiet: call    io_report
        call    io_audio_close
.out:   retf

; =============================================================================
;  The tables
; =============================================================================
build_gdt:
        push    es
        push    cs
        pop     es
        mov     di, gdt
        xor     eax, eax
        mov     cx, GDT_ENTRIES * 4
        rep     stosw
        mov     di, gdt + SEL_CODE32
        mov     eax, [host_base]
        mov     bl, 0x9A
        mov     bh, 0xCF
        call    put_desc
        mov     di, gdt + SEL_DATA32
        mov     bl, 0x92
        mov     bh, 0xCF
        call    put_desc
        mov     di, gdt + SEL_FLAT
        xor     eax, eax
        mov     bl, 0x92
        mov     bh, 0xCF
        call    put_desc
        mov     di, gdt + SEL_CODE16
        mov     eax, [host_base]
        mov     bl, 0x9A
        mov     bh, 0x00
        call    put_desc
        mov     di, gdt + SEL_DATA16
        mov     bl, 0x92
        mov     bh, 0x00
        call    put_desc
        mov     di, gdt + SEL_TSS
        mov     eax, [host_base]
        add     eax, tss
        mov     [di+2], ax
        shr     eax, 16
        mov     [di+4], al
        mov     [di+7], ah
        mov     word [di], TSS_SIZE - 1
        mov     byte [di+5], 0x89               ; an available 32-bit TSS
        mov     byte [di+6], 0
        mov     eax, [host_base]
        add     eax, gdt
        mov     [gdtr+2], eax
        mov     eax, [host_base]
        add     eax, idt
        mov     [idtr+2], eax
        pop     es
        ret

; put_desc: DI -> descriptor, EAX = base, BL = access, BH = flags
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

; build_idt: 256 interrupt gates into the stubs.  DPL 3, because a program in
;   virtual-8086 mode runs at ring 3 and its INTs must be allowed through when
;   the processor has no redirection bitmap to use instead.
build_idt:
        push    es
        push    cs
        pop     es
        mov     di, idt
        xor     cx, cx
.gate:  mov     ax, cx
        imul    ax, VEC_STUB_SIZE
        add     ax, vec_stubs
        stosw
        mov     ax, SEL_CODE32
        stosw
        mov     ax, 0xEE00                      ; present, DPL 3, 32-bit gate
        stosw
        xor     ax, ax
        stosw
        inc     cx
        cmp     cx, 256
        jb      .gate
        pop     es
        ret

; build_tss: a ring-0 stack, an empty redirection bitmap, and a permission
;   map that refuses exactly the ports the card answers to
build_tss:
        push    es
        push    cs
        pop     es
        mov     di, tss
        mov     cx, TSS_SIZE / 2
        xor     ax, ax
        rep     stosw
        ; SS0 is the module's own data selector, not the flat one.  Everything
        ; the monitor touches by name - its stack, its tables, and the
        ; synthesiser's whole world - is an offset in the module, and C code
        ; wants SS and DS based the same way.  GS stays flat, for the
        ; program's memory and the audio ring.
        mov     word [tss + 8], SEL_DATA32      ; SS0
        mov     dword [tss + 4], r0_stack_top   ; ESP0
        mov     word [tss + 102], TSS_IOPB
        mov     byte [tss + TSS_IOPB + IOPB_BYTES], 0xFF
        pop     es
        call    io_trap_setup
        call    io_trap_pic
        call    io_trap_speaker
        ret

; =============================================================================
; v86_enter: from real mode into virtual-8086 mode, and straight back to the
;   caller.  Nothing the caller can see changes: the same registers, the same
;   stack, the same next instruction.  Only the privilege level, and the
;   permission map that comes with it.
; =============================================================================
v86_enter:
        cli
        mov     [cs:ent_eax], eax
        mov     [cs:ent_ebx], ebx
        mov     [cs:ent_ecx], ecx
        mov     [cs:ent_edx], edx
        mov     [cs:ent_esi], esi
        mov     [cs:ent_edi], edi
        mov     [cs:ent_ebp], ebp
        mov     [cs:ent_ds], ds
        mov     [cs:ent_es], es
        mov     [cs:ent_fs], fs
        mov     [cs:ent_gs], gs
        pop     ax                              ; where to carry on
        mov     [cs:ent_ip], ax
        mov     [cs:ent_cs], cs
        mov     [cs:ent_ss], ss
        mov     [cs:ent_sp], sp
        mov     ax, cs
        mov     ds, ax
        call    pic_remap                       ; out of the exceptions' way
        call    rtc_start                       ; and a clock to make sound by
        lgdt    [gdtr]
        lidt    [idtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     SEL_CODE32:pm_start

[BITS 32]
pm_start:
        mov     ax, SEL_DATA32
        mov     ds, ax
        mov     es, ax                          ; ES addresses the module too
        mov     ss, ax
        mov     esp, r0_stack_top
        mov     ax, SEL_FLAT
        mov     fs, ax
        mov     gs, ax
        push    dword 2                         ; NT above all: an IRET with
        popfd                                   ;  NT set is a task return
        ; The processor marks a task busy when it is loaded and never marks it
        ; free again, because a task is normally left by a task switch and
        ; this one is left by walking out of protected mode.  So the second
        ; program to run would meet a busy task and LTR would refuse it.
        and     byte [gdt + SEL_TSS + 5], ~2
        mov     ax, SEL_TSS
        ltr     ax
        ; the coprocessor, for a game that uses it
        mov     eax, cr0
        and     eax, ~0x0C                      ; EM=0, TS=0
        or      eax, 0x22                       ; MP=1, NE=1
        mov     cr0, eax
        fninit
%ifdef HAVE_OPL
        ; the synthesiser, switched on at whatever rate the stream plays
        mov     eax, [au_rate]
        or      eax, eax
        jnz     .have_rate
        mov     eax, 44100
.have_rate:
        push    eax
        call    OPL_RESET
        add     esp, 4
%endif
        mov     word [v_flags], 0
        mov     byte [in_v86], 1
        ; the frame an IRET into virtual-8086 mode wants
        movzx   eax, word [ent_gs]
        push    eax
        movzx   eax, word [ent_fs]
        push    eax
        movzx   eax, word [ent_ds]
        push    eax
        movzx   eax, word [ent_es]
        push    eax
        movzx   eax, word [ent_ss]
        push    eax
        movzx   eax, word [ent_sp]
        push    eax
        push    dword V86_FLAGS
        movzx   eax, word [ent_cs]
        push    eax
        movzx   eax, word [ent_ip]
        push    eax
        mov     ebx, [ent_ebx]
        mov     ecx, [ent_ecx]
        mov     edx, [ent_edx]
        mov     esi, [ent_esi]
        mov     edi, [ent_edi]
        mov     ebp, [ent_ebp]
        mov     eax, [ent_eax]
        iretd

[BITS 16]
; =============================================================================
; v86_leave: asked for from inside virtual-8086 mode, by writing to a port
;   that no machine has ever had.  The monitor sees the fault, takes the
;   processor out of virtual-8086 mode, and puts execution back here.
; =============================================================================
v86_leave:
        push    ax
        push    dx
        mov     dx, IO_ESCAPE
        mov     al, 0x86
        out     dx, al                          ; ...and real mode resumes here
        pop     dx
        pop     ax
        ret


; =============================================================================
;  The stubs: one per vector, so the frame is always the same shape
; =============================================================================
[BITS 32]
                align 4
vec_stubs:
%assign v 0
%rep 256
  %if (v==8)||(v==10)||(v==11)||(v==12)||(v==13)||(v==14)||(v==17)
        push    strict dword v                  ; the processor pushed an error
  %else
        push    strict byte 0                   ; ...and here it did not
        push    strict dword v
  %endif
        jmp     near v86_common
        times   VEC_STUB_SIZE - ($ - vec_stubs - v * VEC_STUB_SIZE) db 0x90
  %assign v v+1
%endrep

v86_common:
        pushad
        sub     esp, 16                         ; where the DPMI host keeps
        mov     ebp, esp                        ;  the segments it pushes
        mov     ax, SEL_DATA32
        mov     ds, ax
        mov     es, ax                          ; ES is the module's, so that
        mov     ax, SEL_FLAT                    ;  a REP STOS by name works
        mov     fs, ax
        mov     gs, ax
        cld
        call    v86_dispatch
        add     esp, 16
        popad
        add     esp, 8                          ; the vector and its error code
        iretd

; -----------------------------------------------------------------------------
; v86_dispatch: what arrived, and what to do about it
; -----------------------------------------------------------------------------
v86_dispatch:
        test    dword [ebp + F_EFL], EFL_VM
        jz      near monitor_fault              ; not the program: this monitor
        movzx   eax, byte [ebp + F_VEC]
        cmp     al, PIC_S_BASE + 8
        jae     near program_fault              ; nothing is sent here
        cmp     al, PIC_M_BASE
        jae     .hardware
        cmp     al, 13
        je      .gp
        ; The ones an 8086 program has always had handlers for go to them:
        ; divide by zero, single step, breakpoint, overflow, bound, an opcode
        ; this processor does not know, and the coprocessor's own.
        cmp     al, 8
        jae     near program_fault
        jmp     .reflect
.gp:
        ; Everything a program is no longer allowed to do arrives here: the
        ; instructions that touch the interrupt flag, the ones that make an
        ; interrupt or return from one, and the ports the permission map
        ; refuses.  Read the instruction and do it on the program's behalf.
        movzx   esi, word [ebp + F_CS]
        shl     esi, 4
        movzx   eax, word [ebp + F_EIP]
        add     esi, eax                        ; where it is, linearly
        ; EBX is the offset of the byte being looked at, and nothing else.
        ; The operand-size prefix used to be remembered in BH, which is the
        ; top half of that same offset: one 66h and every byte after it was
        ; fetched from 256 bytes further on.  Everything without a 66h in
        ; front of it worked, which is nearly everything a program writes -
        ; and not the BIOS, whose PUSHFD is 66 9C.
        mov     byte [op66], 0
        xor     ebx, ebx                        ; the byte being looked at
.prefix:
        mov     al, [gs:esi + ebx]
        cmp     al, 0x66
        je      .p66
        cmp     al, 0x67
        je      .pnext
        cmp     al, 0xF0
        je      .pnext
        cmp     al, 0xF2
        je      .pnext
        cmp     al, 0xF3
        je      .pnext
        cmp     al, 0x26
        je      .pnext
        cmp     al, 0x2E
        je      .pnext
        cmp     al, 0x36
        je      .pnext
        cmp     al, 0x3E
        je      .pnext
        cmp     al, 0x64
        je      .pnext
        cmp     al, 0x65
        je      .pnext
        jmp     .opcode
.p66:   mov     byte [op66], 1
.pnext: inc     bl
        cmp     bl, 8
        jb      .prefix
        jmp     near program_fault              ; nothing sane is this long
.opcode:
        inc     bl                              ; the opcode's own byte
        cmp     al, 0xFA
        je      .cli
        cmp     al, 0xFB
        je      .sti
        cmp     al, 0x9C
        je      .pushf
        cmp     al, 0x9D
        je      .popf
        cmp     al, 0xCD
        je      .int_n
        cmp     al, 0xCC
        je      .int3
        cmp     al, 0xCE
        je      .into
        cmp     al, 0xCF
        je      .iret
        cmp     al, 0xF4                        ; HLT: a program waiting for a
        je      .step                           ;  tick, which comes anyway
        cmp     al, 0x0F
        je      .two_byte
        call    io_emulate                      ; a port, then
        jc      near program_fault
        cmp     byte [io_leave], 0
        jne     near back_to_real
        ret

; ---- a program that means to run the processor itself -----------------------
;  Loading a descriptor table or a control register is how a program leaves
;  real mode behind, and nothing in virtual-8086 mode can stand in for that.
;  So the monitor stands down instead: back to real mode, resuming *at* the
;  instruction rather than after it, and the program has the machine.  It
;  gets no card that way, but it runs, which matters more - and it means a
;  32-bit program of ours is safe even where the kernel does not say so.
.two_byte:
        mov     al, [gs:esi + ebx]              ; the byte after 0Fh
        cmp     al, 0x01                        ; LGDT, LIDT, LMSW and friends
        je      .stand_down
        cmp     al, 0x20                        ; MOV r32, CRn
        je      .stand_down
        cmp     al, 0x22                        ; MOV CRn, r32
        je      .stand_down
        jmp     near program_fault
.stand_down:
        jmp     near back_to_real               ; EIP left where it is

; ---- past whatever it was ---------------------------------------------------
.step:  movzx   eax, bl
        add     eax, [ebp + F_EIP]
        and     eax, 0xFFFF
        mov     [ebp + F_EIP], eax
        ret

.cli:   and     dword [ebp + F_EFL], ~EFL_IF
        jmp     .step
.sti:   or      dword [ebp + F_EFL], EFL_IF
        jmp     .step

; ---- the flags, as the program is allowed to see and set them ---------------
.pushf:
        mov     eax, [ebp + F_EFL]
        and     eax, EFL_PROGRAM                ; nothing about where it is
        or      eax, 2
        or      ax, [v_flags]                   ; ...and what it thinks it set
        cmp     byte [op66], 0
        je      .pushf16
        push    eax
        shr     eax, 16
        call    v86_push
        pop     eax
.pushf16:
        call    v86_push
        jmp     .step
.popf:
        call    v86_pop
        movzx   ecx, ax
        cmp     byte [op66], 0
        je      .popf_have
        call    v86_pop                         ; the half nobody reads
.popf_have:
        mov     eax, ecx
        and     ax, EFL_VIRTUAL                 ; believed, not obeyed
        mov     [v_flags], ax
        mov     eax, ecx
        and     eax, EFL_PROGRAM
        or      eax, EFL_VM | 2
        mov     [ebp + F_EFL], eax
        jmp     .step

; ---- making an interrupt, and coming back from one --------------------------
.int_n:
        movzx   eax, byte [gs:esi + ebx]        ; the vector, after the opcode
        inc     bl
        jmp     .make_int
.int3:  mov     eax, 3
        jmp     .make_int
.into:  test    dword [ebp + F_EFL], EFL_OF
        jz      .step
        mov     eax, 4
.make_int:
        push    eax
        call    .step                           ; the handler returns past it
        pop     eax
        call    v86_reflect
        ret
.iret:
        call    v86_pop
        movzx   ecx, ax                         ; IP
        cmp     byte [op66], 0
        je      .iret_cs
        call    v86_pop
.iret_cs:
        call    v86_pop
        movzx   edx, ax                         ; CS
        cmp     byte [op66], 0
        je      .iret_fl
        call    v86_pop
.iret_fl:
        call    v86_pop
        movzx   eax, ax                         ; and the flags
        cmp     byte [op66], 0
        je      .iret_set
        push    eax
        call    v86_pop
        pop     eax
.iret_set:
        mov     [ebp + F_EIP], ecx
        mov     [ebp + F_CS], edx
        push    eax
        and     ax, EFL_VIRTUAL                 ; believed, not obeyed
        mov     [v_flags], ax
        pop     eax
        and     eax, EFL_PROGRAM
        or      eax, EFL_VM | 2
        mov     [ebp + F_EFL], eax
        ret
        ret
; ---- a hardware interrupt: the program's own handler gets it ----------------
.hardware:
        sub     al, PIC_M_BASE
        mov     [irq_line], al
        cmp     al, RTC_IRQ                     ; the clock this monitor asked
        jne     .not_ours                       ;  for is not the program's
        ; Register C, read, is what tells the chip its interrupt has been
        ; noticed; without it there is one and never another.  Inline, because
        ; the same bytes assembled for sixteen bits mean something else here.
        mov     al, 0x0C
        out     0x70, al
        out     0x80, al
        in      al, 0x71
        call    io_pump
        mov     al, 0x20                        ; both controllers, ourselves
        out     0xA0, al
        out     0x20, al
        ret
.not_ours:
        or      al, al
        jnz     .not_timer
        call    io_pump                         ; the timer keeps it fed too
.not_timer:
        movzx   eax, byte [irq_line]
        cmp     al, 8
        jb      .master
        add     al, 0x70 - 8                    ; the second controller's eight
        jmp     .reflect
.master:
        add     al, 8                           ; where the BIOS put them
.reflect:
        call    v86_reflect
        cmp     byte [irq_line], 0
        jne     .done
        cmp     byte [ebp + F_VEC], PIC_M_BASE  ; only after a real timer tick
        jne     .done
        call    card_interrupt
.done:  ret

; -----------------------------------------------------------------------------
; v86_push / v86_pop: a word on or off the program's own stack, which is a
;   16-bit one wherever it points
; -----------------------------------------------------------------------------
v86_push:
        push    ecx
        push    edx
        movzx   edx, word [ebp + F_SS]
        shl     edx, 4
        mov     ecx, [ebp + F_ESP]
        sub     cx, 2
        and     ecx, 0xFFFF
        mov     [gs:edx + ecx], ax
        mov     [ebp + F_ESP], ecx
        pop     edx
        pop     ecx
        ret

v86_pop:
        push    ecx
        push    edx
        movzx   edx, word [ebp + F_SS]
        shl     edx, 4
        mov     ecx, [ebp + F_ESP]
        and     ecx, 0xFFFF
        mov     ax, [gs:edx + ecx]
        add     cx, 2
        and     ecx, 0xFFFF
        mov     [ebp + F_ESP], ecx
        pop     edx
        pop     ecx
        ret

; -----------------------------------------------------------------------------
; v86_reflect: AL = an 8086 interrupt vector.  Build the frame its handler
;   expects on the program's own stack and point the frame at it, so that
;   returning to virtual-8086 mode arrives inside the handler and its IRET
;   comes back to whatever was interrupted.  Called twice over for a nested
;   one, which is what the card's interrupt inside the timer's is.
; -----------------------------------------------------------------------------
v86_reflect:
        pushad
        movzx   eax, al
        movzx   edx, word [ebp + F_SS]
        shl     edx, 4                          ; the stack, linearly
        mov     ecx, [ebp + F_ESP]
        sub     cx, 6                           ; three words, as an 8086 does
        and     ecx, 0xFFFF
        mov     esi, [ebp + F_EFL]
        mov     [gs:edx + ecx + 4], si
        mov     esi, [ebp + F_CS]
        mov     [gs:edx + ecx + 2], si
        mov     esi, [ebp + F_EIP]
        mov     [gs:edx + ecx], si
        mov     [ebp + F_ESP], ecx
        mov     esi, [gs:eax*4]                 ; the vector itself
        movzx   ebx, si
        mov     [ebp + F_EIP], ebx
        shr     esi, 16
        mov     [ebp + F_CS], esi
        and     dword [ebp + F_EFL], ~(EFL_IF | EFL_TF | EFL_AC)
        popad
        ret

; -----------------------------------------------------------------------------
; card_interrupt: a block of samples has been played, so the card would have
;   raised its own interrupt.  Nothing did - no chip is on that line - so it
;   is raised here, nested inside the timer's whose tick noticed it.  The
;   program enters its own handler for the card first and its timer handler
;   after, which is exactly the order a real one would have arrived in.
; -----------------------------------------------------------------------------
CARD_IRQ        equ 5
CARD_VECTOR     equ 8 + CARD_IRQ
card_interrupt:
        cmp     byte [pic_isr], 0
        je      .idle
        ; The program has not finished with the last one.  Give it a few
        ; ticks, in case it has no handler at all and simply returned.
        inc     byte [isr_age]
        cmp     byte [isr_age], 4
        jb      .none
        mov     byte [pic_isr], 0
.idle:
        cmp     byte [sb_irq_owed], 0
        je      .none
        mov     al, [pic_mask]
        test    al, 1 << CARD_IRQ               ; has the program asked for it?
        jnz     .none
        mov     byte [sb_irq_owed], 0
        mov     byte [pic_isr], 1
        mov     byte [isr_age], 0
        mov     byte [sb_card_irq], 1
        inc     dword [card_irqs]
        mov     al, CARD_VECTOR
        call    v86_reflect
.none:  ret

; =============================================================================
; back_to_real: out of virtual-8086 mode, carrying on where the program was.
;   Everything it holds is in the frame; nothing else needs saying.
; =============================================================================
back_to_real:
        mov     byte [io_leave], 0
        mov     byte [in_v86], 0
        mov     eax, [ebp + F_EAX]
        mov     [ret_eax], eax
        mov     eax, [ebp + F_EBX]
        mov     [ret_ebx], eax
        mov     eax, [ebp + F_ECX]
        mov     [ret_ecx], eax
        mov     eax, [ebp + F_EDX]
        mov     [ret_edx], eax
        mov     eax, [ebp + F_ESI]
        mov     [ret_esi], eax
        mov     eax, [ebp + F_EDI]
        mov     [ret_edi], eax
        mov     eax, [ebp + F_EBP]
        mov     [ret_ebp], eax
        mov     eax, [ebp + F_EIP]
        mov     [ret_ip], ax
        mov     eax, [ebp + F_CS]
        mov     [ret_cs], ax
        mov     eax, [ebp + F_EFL]
        and     eax, ~EFL_IOPL3                 ; ring 3 was this monitor's idea
        or      eax, EFL_IF                     ; and the program had them on
        mov     [ret_fl], ax
        mov     eax, [ebp + F_ESP]
        mov     [ret_sp], ax
        mov     eax, [ebp + F_SS]
        mov     [ret_ss], ax
        mov     eax, [ebp + F_DS]
        mov     [ret_ds], ax
        mov     eax, [ebp + F_ES]
        mov     [ret_es], ax
        mov     eax, [ebp + F_FS]
        mov     [ret_fs], ax
        mov     eax, [ebp + F_GS]
        mov     [ret_gs], ax
        jmp     SEL_CODE16:leave_pm

[BITS 16]
leave_pm:
        mov     ax, SEL_DATA16
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        ; SS above all.  A segment register keeps the descriptor it was last
        ; loaded with until it is loaded again, and real mode does not change
        ; that: it only rewrites the base.  SEL_FLAT is a *big* segment, and
        ; a big stack segment means PUSH, POP, CALL and RET use ESP, not SP -
        ; in real mode, with whatever the ring-0 stack left in the high half.
        ; Loading a 16-bit descriptor here is what makes SP mean SP again.
        mov     ss, ax
        xor     esp, esp
        mov     eax, cr0
        and     al, 0xFE
        mov     cr0, eax
        jmp     far [cs:rm_ptr]                 ; CS = this segment again

rm_resume:
        mov     ax, cs
        mov     ds, ax
        lidt    [idtr_real]
        mov     ss, [ret_ss]                    ; the program's own stack, back
        mov     sp, [ret_sp]                    ;  before anything is called
        call    rtc_stop
        call    pic_restore
        push    word [cs:ret_fl]
        push    word [cs:ret_cs]
        push    word [cs:ret_ip]
        mov     eax, [cs:ret_eax]
        mov     ebx, [cs:ret_ebx]
        mov     ecx, [cs:ret_ecx]
        mov     edx, [cs:ret_edx]
        mov     esi, [cs:ret_esi]
        mov     edi, [cs:ret_edi]
        mov     ebp, [cs:ret_ebp]
        mov     es, [cs:ret_es]
        mov     fs, [cs:ret_fs]
        mov     gs, [cs:ret_gs]
        mov     ds, [cs:ret_ds]
        iret

; =============================================================================
;  When it goes wrong
; =============================================================================
[BITS 32]
; program_fault: an exception the program cannot have meant.  Leave virtual-
;   8086 mode the ordinary way, then say where it was and end the program.
program_fault:
        ; The eight bytes at the fault, because "exception 13" says only that
        ; something was refused and not what: an instruction this monitor does
        ; not emulate looks exactly like one it does.
        movzx   esi, word [ebp + F_CS]
        shl     esi, 4
        movzx   eax, word [ebp + F_EIP]
        add     esi, eax
        mov     eax, [gs:esi]
        mov     [fault_code], eax
        mov     eax, [gs:esi + 4]
        mov     [fault_code + 4], eax
        mov     eax, [ebp + F_VEC]
        mov     [fault_vec], eax
        mov     eax, [ebp + F_ERR]
        mov     [fault_err], eax
        mov     eax, [ebp + F_EIP]
        mov     [fault_ip], eax
        mov     eax, [ebp + F_CS]
        mov     [fault_cs], eax
        mov     byte [fault_kind], 1
        jmp     back_to_fault

; monitor_fault: the frame says the fault was not in virtual-8086 mode, so it
;   was in this monitor - a bug here, not in the program.  Say so plainly:
;   the frame has no program stack in it and nothing can be resumed.
monitor_fault:
        mov     eax, [ebp + F_VEC]
        mov     [fault_vec], eax
        mov     eax, [ebp + F_ERR]
        mov     [fault_err], eax
        mov     eax, [ebp + F_EIP]
        mov     [fault_ip], eax
        mov     eax, [ebp + F_CS]
        mov     [fault_cs], eax
        mov     byte [fault_kind], 2
        jmp     back_to_fault

back_to_fault:
        mov     byte [in_v86], 0
        jmp     SEL_CODE16:leave_pm_fault

[BITS 16]
leave_pm_fault:
        mov     ax, SEL_DATA16
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax                          ; SP means SP again (leave_pm)
        xor     esp, esp
        mov     eax, cr0
        and     al, 0xFE
        mov     cr0, eax
        jmp     far [cs:rm_fault_ptr]

; rm_nibble: the low four bits of AL, as a character
rm_nibble:
        and     al, 0x0F
        cmp     al, 10
        jb      .digit
        add     al, 7
.digit: add     al, '0'
        SVC     SVC_PUTC
        ret

rm_fault:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, rm_stack_top                ; our own, whatever was left
        lidt    [idtr_real]
        call    rtc_stop
        call    pic_restore
        sti
        ; Text mode first.  A program that faults has usually put the screen
        ; into a graphics mode of its own, and a report written onto that is
        ; a report nobody reads: the shell puts the screen back on the way
        ; out and takes the report with it.  Setting mode 3 here means the
        ; shell finds it already set and leaves the screen alone.
        mov     ax, 0x0003
        int     0x10
        call    io_report
        call    io_audio_close
        mov     si, msg_fault
        cmp     byte [fault_kind], 2
        jne     .say
        mov     si, msg_fault_here
.say:   SVC     SVC_PUTS
        mov     eax, [fault_vec]
        SVC     SVC_PRINT_DEC
        mov     si, msg_fault_at
        SVC     SVC_PUTS
        mov     ax, [fault_cs]
        SVC     SVC_PRINT_HEX16
        mov     al, ':'
        SVC     SVC_PUTC
        mov     ax, [fault_ip]
        SVC     SVC_PRINT_HEX16
        mov     si, msg_fault_err
        SVC     SVC_PUTS
        mov     ax, [fault_err]
        SVC     SVC_PRINT_HEX16
        cmp     byte [fault_kind], 2
        je      .no_code
        mov     si, msg_fault_code
        SVC     SVC_PUTS
        xor     bx, bx
.byte:  mov     al, [fault_code + bx]
        push    bx
        mov     bl, al
        shr     al, 4
        call    rm_nibble
        mov     al, bl
        call    rm_nibble
        mov     al, ' '
        SVC     SVC_PUTC
        pop     bx
        inc     bx
        cmp     bx, 8
        jb      .byte
.no_code:
        SVC     SVC_CRLF
        mov     si, msg_fault
        mov     eax, [fault_ip]
        SVC     SVC_LOG_LINE
        mov     ax, 0x4CFF
        int     0x21
        jmp     $

; =============================================================================
;  The interrupt controller, moved and moved back
; =============================================================================
; pic_remap: the eight hardware requests onto vectors 20h-2Fh, so nothing can
;   be confused with an exception.  The masks are kept, and the card's line is
;   masked in the hardware whatever the program later asks for: no chip is on
;   it, and the only interrupts that arrive there are the ones this monitor
;   raises itself.
pic_remap:
        push    ax
        in      al, 0x21
        mov     [pic_saved_m], al
        or      al, 1 << CARD_IRQ
        mov     [pic_want_m], al
        mov     [pic_mask], al
        in      al, 0xA1
        mov     [pic_saved_s], al
        mov     al, 0x11                        ; begin: cascade, four words
        out     0x20, al
        call    io_settle
        out     0xA0, al
        call    io_settle
        mov     al, PIC_M_BASE
        out     0x21, al
        call    io_settle
        mov     al, PIC_S_BASE
        out     0xA1, al
        call    io_settle
        mov     al, 0x04                        ; the second one is on line 2
        out     0x21, al
        call    io_settle
        mov     al, 0x02
        out     0xA1, al
        call    io_settle
        mov     al, 0x01                        ; 8086 mode
        out     0x21, al
        call    io_settle
        out     0xA1, al
        call    io_settle
        mov     al, [pic_want_m]
        out     0x21, al
        call    io_settle
        mov     al, [pic_saved_s]
        out     0xA1, al
        call    io_settle
        pop     ax
        ret

; -----------------------------------------------------------------------------
; rtc_start / rtc_stop: the real-time chip's periodic interrupt, borrowed.
;   Register A holds the rate, register B switches it on, and register C has
;   to be read after every one or no more arrive.  What was there before is
;   put back on the way out, because the chip is also where the time of day
;   comes from and this is only a loan.
; -----------------------------------------------------------------------------
rtc_start:
        push    ax
        cli
        mov     al, 0x8A                        ; register A, and no NMI
        out     0x70, al
        call    io_settle
        in      al, 0x71
        mov     [rtc_saved_a], al
        and     al, 0xF0
        or      al, RTC_RATE
        mov     ah, al
        mov     al, 0x8A
        out     0x70, al
        call    io_settle
        mov     al, ah
        out     0x71, al
        call    io_settle
        mov     al, 0x8B                        ; register B: the periodic bit
        out     0x70, al
        call    io_settle
        in      al, 0x71
        mov     [rtc_saved_b], al
        or      al, 0x40
        mov     ah, al
        mov     al, 0x8B
        out     0x70, al
        call    io_settle
        mov     al, ah
        out     0x71, al
        call    io_settle
        call    rtc_ack                         ; anything already pending
        in      al, 0xA1                        ; and let the line through
        and     al, ~(1 << RTC_IRQ_SLAVE)
        out     0xA1, al
        mov     byte [rtc_on], 1
        mov     dword [au_lead_start], AU_LEAD_FAST ; four ms of clock
        mov     dword [au_lead], AU_LEAD_FAST
        pop     ax
        ret

rtc_stop:
        push    ax
        cmp     byte [rtc_on], 0
        je      .done
        mov     byte [rtc_on], 0
        mov     dword [au_lead_start], AU_LEAD_SLOW
        mov     dword [au_lead], AU_LEAD_SLOW
        cli
        mov     al, 0x8B                        ; the periodic bit, back as found
        out     0x70, al
        call    io_settle
        mov     al, [rtc_saved_b]
        out     0x71, al
        call    io_settle
        mov     al, 0x8A
        out     0x70, al
        call    io_settle
        mov     al, [rtc_saved_a]
        out     0x71, al
        call    io_settle
        mov     al, 0x0D                        ; leave the index somewhere
        out     0x70, al                        ;  harmless, as the BIOS does
.done:  pop     ax
        ret

; rtc_ack: read register C, which is what tells the chip its interrupt has
;   been noticed.  Without this it raises one and never another.
rtc_ack:
        push    ax
        mov     al, 0x0C
        out     0x70, al
        call    io_settle
        in      al, 0x71
        pop     ax
        ret

; pic_restore: back to 08h and 70h, where the BIOS left it and where every
;   real-mode handler in this machine expects to be called from
pic_restore:
        push    ax
        mov     al, 0x11
        out     0x20, al
        call    io_settle
        out     0xA0, al
        call    io_settle
        mov     al, 0x08
        out     0x21, al
        call    io_settle
        mov     al, 0x70
        out     0xA1, al
        call    io_settle
        mov     al, 0x04
        out     0x21, al
        call    io_settle
        mov     al, 0x02
        out     0xA1, al
        call    io_settle
        mov     al, 0x01
        out     0x21, al
        call    io_settle
        out     0xA1, al
        call    io_settle
        mov     al, [pic_saved_m]               ; exactly as it was found
        out     0x21, al
        call    io_settle
        mov     al, [pic_saved_s]
        out     0xA1, al
        call    io_settle
        pop     ax
        ret

; io_settle: a real controller wants a moment between writes; this machine's
;   is emulated in firmware and wants rather more than a moment
io_settle:
        push    ax
        out     0x80, al
        out     0x80, al
        pop     ax
        ret

%include "dpmi_io.inc"
%include "io_decode.inc"

; =============================================================================
;  Everything below is data, but it stays in the one section: a flat binary
;  laid out in the order it is written is what lets the synthesiser be placed
;  at a known offset with TIMES, and a second section makes that offset
;  something NASM will not work out until too late.
; =============================================================================
                align 8
gdt:            times GDT_ENTRIES * 8 db 0
gdtr:           dw GDT_ENTRIES * 8 - 1
                dd 0
idtr:           dw 256 * 8 - 1
                dd 0
idtr_real:      dw 0x03FF
                dd 0
rm_ptr:         dw rm_resume
rm_seg:         dw 0
rm_fault_ptr:   dw rm_fault
rm_fault_seg:   dw 0
host_base:      dd 0
r0_top:         dd 0
in_v86:         db 0
irq_line:       db 0
op66:           db 0                            ; an operand-size prefix seen
                align 2
v_flags:        dw 0                            ; flags a program believes in
isr_age:        db 0
fault_kind:     db 0
                align 4
card_irqs:      dd 0
fault_vec:      dd 0
fault_err:      dd 0
fault_ip:       dd 0
fault_cs:       dd 0
pic_saved_m:    db 0
pic_saved_s:    db 0
pic_want_m:     db 0
rtc_on:         db 0
rtc_saved_a:    db 0
rtc_saved_b:    db 0
                align 4
; the machine as it was when a program started, and as it is when one ends
ent_eax:        dd 0
ent_ebx:        dd 0
ent_ecx:        dd 0
ent_edx:        dd 0
ent_esi:        dd 0
ent_edi:        dd 0
ent_ebp:        dd 0
ent_ds:         dw 0
ent_es:         dw 0
ent_fs:         dw 0
ent_gs:         dw 0
ent_ss:         dw 0
ent_sp:         dw 0
ent_cs:         dw 0
ent_ip:         dw 0
                align 4
ret_eax:        dd 0
ret_ebx:        dd 0
ret_ecx:        dd 0
ret_edx:        dd 0
ret_esi:        dd 0
ret_edi:        dd 0
ret_ebp:        dd 0
ret_ds:         dw 0
ret_es:         dw 0
ret_fs:         dw 0
ret_gs:         dw 0
ret_ss:         dw 0
ret_sp:         dw 0
ret_cs:         dw 0
ret_ip:         dw 0
ret_fl:         dw 0
svc_table:      times SVC_MAX * 4 db 0
msg_loaded:     db "SB: a Sound Blaster at 220h, IRQ 5, channel 1, for DOS "
                db "programs", 13, 10, 0
msg_fault:      db "SB: the program stopped at exception ", 0
msg_fault_here: db "SB: the monitor itself faulted, exception ", 0
msg_back:       db "SB: back in real mode", 13, 10, 0
msg_fault_at:   db " at ", 0
msg_fault_err:  db " error ", 0
msg_fault_code: db ", on the bytes ", 0
                align 4
fault_code:     times 8 db 0
msg_irqs:       db "SB: the card own interrupts  ", 0

; =============================================================================
                align 8
tss:            times TSS_SIZE db 0
                align 8
idt:            times 256 * 8 db 0
rm_stack:       times 1024 db 0
rm_stack_top:
                align 16
r0_stack:       times 3072 db 0
r0_stack_top:
bss_end:

; =============================================================================
;  The synthesiser
; -----------------------------------------------------------------------------
;  nano/opl/opl3.c, compiled freestanding and linked at OPL_ORG - which is an
;  offset in this module, and this module's selectors are based on this
;  module, so every address in it is right wherever the kernel puts us.  That
;  is the whole trick: no fixed physical address, no relocation, no second
;  module to find.  tools/build_opl.py makes it and says where things are.
;
;  What follows the image is the memory it was linked to have and does not
;  carry: the chip's state, which opl_reset fills in.
; =============================================================================
%ifdef HAVE_OPL
                times (OPL_ORG - MOD_ORG) - ($ - $$) db 0
opl_image:      incbin "opl.bin"
                times (OPL_END - MOD_ORG) - ($ - $$) db 0
opl_end:
%endif
