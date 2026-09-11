; =============================================================================
;  shim.asm - the BIOS that Ember 2.0 brings with it
; -----------------------------------------------------------------------------
;  Ember is a real-mode kernel that talks to the BIOS: INT 13h for the disk,
;  INT 10h for the screen, INT 16h for the keyboard, INT 15h for how much
;  memory there is.  A UEFI-only machine has no BIOS.  This is the one it gets
;  instead: a flat binary that the stub (stub.c) puts at the top of
;  conventional memory, fills a header in, and jumps to while the processor is
;  still in long mode.  From there it drops the processor back to real mode,
;  builds an interrupt table that points into itself, and boots Ember off a
;  copy of its disk image that the stub left in memory - and Ember never finds
;  out.
;
;  Three things make it different from the firmware it replaces:
;
;   - The screen is a framebuffer, not a text buffer, and on every machine
;     this was written for it sits above four gigabytes.  Real mode cannot
;     reach it and cannot use paging.  So every character is drawn by hopping
;     into 32-bit protected mode with a page directory that maps the frame-
;     buffer into a window at FBWIN, drawing, and hopping back.  PSE-36 does
;     that with one four-kilobyte page of tables and no PAE.
;   - The disk is a copy of the image in memory above a megabyte.  Reads and
;     writes are copies, made in the same protected-mode hop.  Writes last
;     until the machine is turned off.
;   - It is position independent at a paragraph.  Nothing in here has an
;     absolute address: real-mode code addresses its data through a segment
;     that is wherever the stub put it, and the protected-mode selectors have
;     that same base.  The stub decides where it lives; the machine decides
;     where that can be.
;
;  Assembled with ORG 0.  The stub reads the header at the top to find the
;  entry point and to fill in what it knows.
; =============================================================================

; The window on the framebuffer is eight 4 MB pages of linear address space.
; Where it sits is the stub's choice, in h_fbwin: a block of RAM it set aside
; and nothing else will ever use, so that the physical addresses the window
; hides are nobody's - not a device's registers, not Ember's memory.
FBWIN_PDES      equ 8                   ; eight 4 MB pages: 32 MB of window
VBE_MODE        equ 0x0140              ; the one mode the VESA calls offer
COLS            equ 80
ROWS            equ 25
SHIFT_MAX       equ 8                   ; the screen is at most 8x magnified

SEL_CODE32      equ 0x08                ; flat, for leaving long mode
SEL_DATA32      equ 0x10                ; flat, for the framebuffer and disk
SEL_CODE16      equ 0x18                ; based here, 64 KB, the way back
SEL_DATA16      equ 0x20                ; based here, 64 KB
SEL_SDATA       equ 0x28                ; based here, 4 GB: [var] works
SEL_SCODE       equ 0x30                ; based here, 32-bit: labels work

; the BIOS data area, as every program that ever peeked at it expects
BDA_EQUIP       equ 0x410
BDA_MEMKB       equ 0x413
BDA_SHIFT       equ 0x417
BDA_SHIFT2      equ 0x418
BDA_KBHEAD      equ 0x41A
BDA_KBTAIL      equ 0x41C
BDA_KBBUF       equ 0x41E               ; ..0x43D, sixteen words
BDA_KBEND       equ 0x43E
BDA_VMODE       equ 0x449
BDA_COLS        equ 0x44A
BDA_PAGESZ      equ 0x44C
BDA_CURSOR      equ 0x450               ; page 0: col, row
BDA_CURSHAPE    equ 0x460
BDA_CRTC        equ 0x463
BDA_TICKS       equ 0x46C
BDA_TICKOVF     equ 0x470
BDA_KBSTART     equ 0x480
BDA_KBSTOP      equ 0x482
BDA_ROWS        equ 0x484
BDA_CHARHT      equ 0x485
BDA_EBDA        equ 0x40E

TICKS_PER_DAY   equ 0x1800B0

[ORG 0]

; =============================================================================
;  The header the stub reads and fills
; =============================================================================
header:
        db      "EMB2"                  ; +0
        dd      shim_end                ; +4   how big this is
        dd      entry64                 ; +8   where to jump, relative to +0
        dd      0                       ; +12  (so the quadword below is at 16:
                                        ;  the stub fills by these offsets)
h_fb_base:      dq 0                    ; +16  the framebuffer, physical
h_fb_w:         dd 0                    ; +24  pixels across
h_fb_h:         dd 0                    ; +28  pixels down
h_fb_pitch:     dd 0                    ; +32  pixels per scan line
h_fb_bgr:       dd 0                    ; +36  1: byte order B G R, 0: R G B
h_rd_base:      dd 0                    ; +40  the disk image, physical
h_rd_size:      dd 0                    ; +44  in bytes
h_low_top:      dd 0                    ; +48  end of conventional memory
h_ext_end:      dd 0                    ; +52  end of memory above 1 MB
h_e820_n:       dd 0                    ; +56  entries in the table below
h_phys:         dd 0                    ; +60  where this is (entry64 writes)
h_fbwin:        dd 0                    ; +64  the window: 32 MB of the stub's
                                        ;  own RAM, 4 MB aligned, whose linear
                                        ;  addresses lead to the framebuffer
                dd 0, 0, 0              ; +68  reserved
h_e820:         times 32 * 24 db 0      ; +80  base, length, type

; =============================================================================
;  Leaving long mode
; -----------------------------------------------------------------------------
;  The stub calls entry64 as a function.  It never returns.  Interrupts off,
;  work out where we are, put that into every descriptor and far pointer that
;  needs it, quieten the interrupt hardware the firmware left running, and
;  step down: 64 -> 32 -> 16 -> real.
; =============================================================================
[BITS 64]
default rel
entry64:
        cli
        lea     rax, [header]           ; the runtime address of offset 0
        mov     [h_phys], eax
        ; the four descriptors based here
        lea     rdi, [gdt_code16]
        call    put_base
        lea     rdi, [gdt_data16]
        call    put_base
        lea     rdi, [gdt_sdata]
        call    put_base
        lea     rdi, [gdt_scode]
        call    put_base
        ; the GDT register's own pointer
        lea     rbx, [gdt]
        mov     [gdtr + 2], ebx
        ; the real-mode segment, into the far pointers that get us there
        mov     ecx, eax
        shr     ecx, 4
        mov     [rm_vec + 2], cx
        mov     [rm_vec2 + 2], cx
        mov     [shim_seg], cx

        call    quieten

        lgdt    [gdtr]
        push    qword SEL_SCODE
        push    qword drop32
        retfq                           ; a far return is a far jump we can
                                        ;  make with a runtime selector

; put_base: EAX = base, RDI -> descriptor.  Bytes 2-4 and 7 carry it.
put_base:
        mov     [rdi + 2], ax
        mov     ebx, eax
        shr     ebx, 16
        mov     [rdi + 4], bl
        mov     [rdi + 7], bh
        ret

; -----------------------------------------------------------------------------
; quieten: the firmware runs with the local APIC on, the I/O APIC routing
;   whatever it routes, and sometimes the HPET standing in for the timer and
;   the clock.  A real-mode kernel wants the 8259s and nothing else, and wants
;   their INTR to reach the core.  Switching the local APIC off makes the
;   processor behave like one that never had it, which is exactly the machine
;   Ember was written for.  The I/O APIC's lines are masked so nothing arrives
;   on a vector no one set up; the HPET is told to stop replacing the timer
;   interrupts the PIT is about to make.
; -----------------------------------------------------------------------------
quieten:
        ; ---- the I/O APIC, if it is where they always are ----
        mov     rsi, 0xFEC00000
        mov     dword [rsi], 1          ; the version register
        mov     eax, [rsi + 0x10]
        cmp     eax, -1
        je      .no_ioapic
        shr     eax, 16
        and     eax, 0xFF               ; the highest entry
        mov     ecx, eax
        inc     ecx
        mov     edx, 0x10               ; the first redirection register
.mask:  mov     [rsi], edx
        mov     eax, [rsi + 0x10]
        or      eax, 1 << 16            ; masked
        mov     [rsi + 0x10], eax
        add     edx, 2
        loop    .mask
.no_ioapic:
        ; ---- the HPET, likewise ----
        mov     rsi, 0xFED00000
        mov     eax, [rsi]
        cmp     eax, -1
        je      .no_hpet
        or      eax, eax
        jz      .no_hpet
        mov     eax, [rsi + 0x10]
        and     eax, ~3                 ; not enabled, not replacing legacy
        mov     [rsi + 0x10], eax
.no_hpet:
        ; ---- the local APIC: off ----
        mov     ecx, 0x1B
        rdmsr
        and     eax, ~(3 << 10)         ; neither the APIC nor x2APIC
        wrmsr
        ret

default abs

; -----------------------------------------------------------------------------
; drop32: compatibility mode, code segment based here.  Paging off ends long
;   mode; then the long mode enable bit itself.
; -----------------------------------------------------------------------------
[BITS 32]
drop32:
        mov     ax, SEL_SDATA
        mov     ds, ax
        mov     ss, ax
        mov     esp, stack_top
        mov     ax, SEL_DATA32
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     eax, cr0
        and     eax, 0x7FFFFFFF         ; PG
        mov     cr0, eax
        mov     ecx, 0xC0000080         ; EFER
        rdmsr
        and     eax, ~(1 << 8)          ; LME
        wrmsr
        mov     eax, cr4
        and     eax, ~(1 << 5)          ; PAE, which we will not use
        mov     cr4, eax
        jmp     SEL_CODE16:drop16

[BITS 16]
drop16:
        mov     ax, SEL_DATA16
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, stack_top
        mov     eax, cr0
        and     al, 0xFE                ; PE
        mov     cr0, eax
        jmp     far [rm_vec]
rm_vec:         dw rm_entry, 0

; =============================================================================
;  Real mode.  Build the machine Ember expects and boot it.
; =============================================================================
rm_entry:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, stack_top
        cld
        ; Real mode still vectors interrupts through IDTR, and IDTR is still
        ; pointing at the firmware's 64-bit table.  The first INT 10h looked
        ; its vector up there and jumped into the dark.  A BIOS leaves this at
        ; 0:3FFh; so does this.
        lidt    [idtr_real]

        call    serial_init
        call    pic_init
        call    pit_init
        call    bda_init
        call    ivt_init
        call    screen_init
        call    kb_drain
        sti

        mov     si, msg_banner
        call    say
        mov     eax, [h_rd_size]
        shr     eax, 20
        call    say_dec
        mov     si, msg_banner2
        call    say
        mov     eax, [h_low_top]
        shr     eax, 10
        call    say_dec
        mov     si, msg_banner3
        call    say
        mov     eax, [h_ext_end]
        shr     eax, 20
        call    say_dec
        mov     si, msg_banner4
        call    say

        ; ---- the first sector of the disk to 7C00h, and go ----
        xor     ax, ax
        mov     es, ax
        mov     bx, 0x7C00
        mov     dword [cp_src], 0
        mov     eax, [h_rd_base]
        mov     [cp_src], eax
        mov     dword [cp_dst], 0x7C00
        mov     dword [cp_len], 512
        call    pm_copy
        mov     dl, 0x80                ; the first hard disk, as far as Ember
        xor     ax, ax                  ;  can tell
        mov     ds, ax
        mov     es, ax
        jmp     0x0000:0x7C00

; -----------------------------------------------------------------------------
; The interrupt controllers, where the BIOS leaves them: 08h and 70h.
; -----------------------------------------------------------------------------
pic_init:
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
        mov     al, 0xF8                ; timer, keyboard, and the cascade
        out     0x21, al
        mov     al, 0xFF
        out     0xA1, al
        ; the IMCR, on a machine old enough to have one: PIC mode
        mov     al, 0x70
        out     0x22, al
        xor     al, al
        out     0x23, al
        ret

; -----------------------------------------------------------------------------
; kb_drain: empty the keyboard controller's output buffer before its line is
;   unmasked.  A byte the firmware left unread keeps the line asserted, the
;   interrupt is edge triggered, and nothing further would ever be reported:
;   a keyboard that works one boot and not the next.  The controller is also
;   told, in so many words, to enable the keyboard, in case the firmware's
;   USB emulation left it otherwise.
; -----------------------------------------------------------------------------
kb_drain:
        push    ax
        push    cx
        mov     cx, 64
.drain: in      al, 0x64
        test    al, 0x01                ; something to read?
        jz      .empty
        in      al, 0x60
        loop    .drain
.empty: mov     cx, 0xFFFF
.ready: in      al, 0x64
        test    al, 0x02                ; ready for a command?
        jz      .send
        loop    .ready
.send:  mov     al, 0xAE                ; enable the keyboard interface
        out     0x64, al
        mov     cx, 0xFFFF
.ready2:
        in      al, 0x64
        test    al, 0x02
        jz      .done
        loop    .ready2
.done:  pop     cx
        pop     ax
        ret

pit_init:
        mov     al, 0x36                ; channel 0, mode 3, 18.2 a second
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        ret

; -----------------------------------------------------------------------------
; The BIOS data area.  Ember reads the keyboard buffer's head and tail out of
; it directly, and the memory size, and the tick count; programs read more.
; -----------------------------------------------------------------------------
bda_init:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     di, 0x400
        mov     cx, 0x80
        rep     stosw
        mov     word [es:BDA_EQUIP], 0x0022     ; 80x25 colour, coprocessor
        mov     eax, [h_low_top]
        shr     eax, 10
        mov     [es:BDA_MEMKB], ax
        mov     ax, [shim_seg]
        mov     [es:BDA_EBDA], ax               ; we are the extended data area
        mov     word [es:BDA_KBHEAD], BDA_KBBUF
        mov     word [es:BDA_KBTAIL], BDA_KBBUF
        mov     word [es:BDA_KBSTART], BDA_KBBUF
        mov     word [es:BDA_KBSTOP], BDA_KBEND
        mov     byte [es:BDA_VMODE], 3
        mov     word [es:BDA_COLS], COLS
        mov     word [es:BDA_PAGESZ], COLS * ROWS * 2
        mov     word [es:BDA_CURSHAPE], 0x0607
        mov     word [es:BDA_CRTC], 0x3D4
        mov     byte [es:BDA_ROWS], ROWS - 1
        mov     word [es:BDA_CHARHT], 16
        mov     byte [es:0x487], 0x60           ; EGA info: 256 KB, colour
        mov     byte [es:0x489], 0x11           ; VGA active, 400 lines
        pop     es
        ret

; -----------------------------------------------------------------------------
; The interrupt vectors.  Every one points somewhere harmless; the hardware
; lines acknowledge themselves; the services Ember uses point at their
; handlers.
; -----------------------------------------------------------------------------
ivt_init:
        push    es
        xor     ax, ax
        mov     es, ax
        xor     di, di
        mov     cx, 256
        mov     ax, int_iret
        mov     dx, cs
.fill:  stosw
        xchg    ax, dx
        stosw
        xchg    ax, dx
        loop    .fill
        ; IRQ 0-7 at 08h-0Fh, 8-15 at 70h-77h
        mov     di, 0x08 * 4
        mov     ax, irq_master
        mov     cx, 8
.m:     stosw
        mov     ax, cs
        stosw
        mov     ax, irq_master
        loop    .m
        mov     di, 0x70 * 4
        mov     ax, irq_slave
        mov     cx, 8
.s:     stosw
        mov     ax, cs
        stosw
        mov     ax, irq_slave
        loop    .s
%macro  VECTOR 2
        mov     word [es:%1 * 4], %2
        mov     [es:%1 * 4 + 2], cs
%endmacro
        VECTOR  0x08, int08
        VECTOR  0x09, int09
        VECTOR  0x10, int10
        VECTOR  0x11, int11
        VECTOR  0x12, int12
        VECTOR  0x13, int13
        VECTOR  0x15, int15
        VECTOR  0x16, int16
        VECTOR  0x18, int19
        VECTOR  0x19, int19
        VECTOR  0x1A, int1A
        VECTOR  0x1F, font_high         ; the 8x8 font pointer, ours 8x16
        VECTOR  0x43, font_high
        pop     es
        ret

font_high:                              ; only ever taken as an address
int_iret:
        iret

irq_master:
        push    ax
        mov     al, 0x20
        out     0x20, al
        pop     ax
        iret

irq_slave:
        push    ax
        mov     al, 0x20
        out     0xA0, al
        out     0x20, al
        pop     ax
        iret

; -----------------------------------------------------------------------------
; INT 08h: the timer.  Count, wrap at midnight, give the user hook its turn.
; -----------------------------------------------------------------------------
int08:
        push    ds
        push    ax
        xor     ax, ax
        mov     ds, ax
        inc     dword [BDA_TICKS]
        cmp     dword [BDA_TICKS], TICKS_PER_DAY
        jb      .no_wrap
        mov     dword [BDA_TICKS], 0
        mov     byte [BDA_TICKOVF], 1
.no_wrap:
        int     0x1C
        ; ---- the heartbeat, while a machine is being understood ----
        ; Once a second, into the top right corner: ticks, keyboard
        ; interrupts, the last scan code, and whether the console has the
        ; screen.  A keyboard that has gone quiet is one of four different
        ; faults, and this is how they are told apart from a photograph.
        inc     byte [cs:hb_count]
        cmp     byte [cs:hb_count], 18
        jb      .no_beat
        mov     byte [cs:hb_count], 0
        cmp     byte [cs:gfx_mode], 0
        jne     .no_beat
        call    heartbeat
.no_beat:
        mov     al, 0x20
        out     0x20, al
        pop     ax
        pop     ds
        iret

; heartbeat: "T:tttttt K:kkkk S:ss" into row 0, columns 60-79, and drawn.
;   Called from the timer with DS = 0.
heartbeat:
        push    ds
        push    es
        push    fs
        pusha
        xor     ax, ax
        mov     fs, ax                  ; the BIOS data area, through FS:
        push    cs                      ;  STOSW writes through ES, which
        pop     ds                      ;  has to be the cells
        push    cs
        pop     es
        mov     di, cells + 60 * 2      ; row 0, column 60
        mov     ah, 0x70                ; black on grey: unmistakable
        mov     al, 'T'
        stosw
        mov     al, ':'
        stosw
        mov     eax, [fs:BDA_TICKS]
        mov     cx, 6
        call    hb_hex
        mov     al, ' '
        mov     ah, 0x70
        stosw
        mov     al, 'K'
        stosw
        mov     al, ':'
        stosw
        movzx   eax, word [kb_irqs]
        mov     cx, 4
        call    hb_hex
        mov     al, ' '
        mov     ah, 0x70
        stosw
        mov     al, 'S'
        stosw
        mov     al, ':'
        stosw
        movzx   eax, byte [kb_last]
        mov     cx, 2
        call    hb_hex
        mov     byte [dr_col], 60
        mov     byte [dr_row], 0
        mov     word [dr_n], 20
        call    pm_draw
        popa
        pop     fs
        pop     es
        pop     ds
        ret

; hb_hex: the low CX nibbles of EAX as hex digits at DS:DI, attribute 70h
hb_hex:
        push    bx
        mov     bx, cx
.digit: dec     bx
        push    eax
        push    cx
        mov     cx, bx
        shl     cx, 2
        shr     eax, cl
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .have
        add     al, 7
.have:  mov     ah, 0x70
        stosw
        pop     cx
        pop     eax
        or      bx, bx
        jnz     .digit
        pop     bx
        ret

; -----------------------------------------------------------------------------
; INT 09h: the keyboard.  Scan codes in, keystrokes into the BIOS buffer, in
;   the encoding every real-mode program expects: the scan code in the high
;   byte and the character, if there is one, in the low.
; -----------------------------------------------------------------------------
int09:
        push    ds
        push    es
        pusha
        mov     ax, cs
        mov     ds, ax
        xor     ax, ax
        mov     es, ax
        in      al, 0x60
        inc     word [kb_irqs]          ; for the heartbeat
        mov     [kb_last], al

        cmp     byte [kb_skip], 0       ; the tail of a Pause sequence
        je      .not_skipping
        dec     byte [kb_skip]
        jmp     .done
.not_skipping:
        cmp     al, 0xE1
        jne     .not_e1
        mov     byte [kb_skip], 5
        jmp     .done
.not_e1:
        cmp     al, 0xE0
        jne     .not_e0
        mov     byte [kb_e0], 1
        jmp     .done
.not_e0:
        mov     bl, al
        and     bl, 0x7F                ; the key
        test    al, 0x80                ; released?
        jnz     .release

        ; ---- pressed: the modifiers first ----
        cmp     bl, 0x2A
        jne     .n1
        or      byte [es:BDA_SHIFT], 0x02
        jmp     .done
.n1:    cmp     bl, 0x36
        jne     .n2
        or      byte [es:BDA_SHIFT], 0x01
        jmp     .done
.n2:    cmp     bl, 0x1D
        jne     .n3
        or      byte [es:BDA_SHIFT], 0x04
        jmp     .done
.n3:    cmp     bl, 0x38
        jne     .n4
        or      byte [es:BDA_SHIFT], 0x08
        jmp     .done
.n4:    cmp     bl, 0x3A
        jne     .n5
        xor     byte [es:BDA_SHIFT], 0x40
        jmp     .done
.n5:    cmp     bl, 0x45
        jne     .n6
        xor     byte [es:BDA_SHIFT], 0x20
        jmp     .done
.n6:    cmp     bl, 0x46
        jne     .n7
        xor     byte [es:BDA_SHIFT], 0x10
        jmp     .done
.n7:
        ; ---- a key with a meaning ----
        movzx   si, bl
        cmp     bl, 0x58
        ja      .done
        mov     dl, [es:BDA_SHIFT]
        cmp     byte [kb_e0], 0
        jne     .extended
        cmp     bl, 0x3B                ; F1..F10
        jb      .ordinary
        cmp     bl, 0x44
        ja      .ordinary
        mov     ah, bl
        test    dl, 0x03
        jz      .f_noshift
        add     ah, 0x19
.f_noshift:
        test    dl, 0x04
        jz      .f_noctrl
        add     ah, 0x23
.f_noctrl:
        test    dl, 0x08
        jz      .f_noalt
        add     ah, 0x2D
.f_noalt:
        xor     al, al
        jmp     .store
.extended:
        ; the grey keys: navigation, no character.  Two exceptions.
        mov     ah, bl
        xor     al, al
        cmp     bl, 0x1C
        jne     .x1
        mov     al, 0x0D                ; keypad Enter
.x1:    cmp     bl, 0x35
        jne     .store
        mov     al, '/'                 ; keypad divide
        jmp     .store
.ordinary:
        mov     ah, bl
        mov     al, [kb_normal + si]
        cmp     bl, 0x47                ; the keypad: digits only with Num Lock
        jb      .not_keypad
        cmp     bl, 0x53
        ja      .not_keypad
        test    dl, 0x20
        jz      .kp_nav
        test    dl, 0x03                ; ...and Shift undoes it
        jz      .store
.kp_nav:
        xor     al, al
        jmp     .store
.not_keypad:
        test    dl, 0x08                ; Alt: the scan code alone
        jz      .not_alt
        xor     al, al
        jmp     .store
.not_alt:
        test    dl, 0x04                ; Ctrl: the control characters
        jz      .not_ctrl
        cmp     al, 'a'
        jb      .ctrl_other
        cmp     al, 'z'
        ja      .ctrl_other
        sub     al, 0x60
        jmp     .store
.ctrl_other:
        cmp     al, '['
        jb      .done
        cmp     al, '_'
        ja      .done
        sub     al, 0x40
        jmp     .store
.not_ctrl:
        test    dl, 0x03
        jz      .no_shift
        mov     al, [kb_shifted + si]
.no_shift:
        test    dl, 0x40                ; Caps Lock: letters the other way
        jz      .store
        mov     cl, al
        or      cl, 0x20
        cmp     cl, 'a'
        jb      .store
        cmp     cl, 'z'
        ja      .store
        xor     al, 0x20
.store:
        ; into the ring: tail moves, unless it would meet the head
        mov     di, [es:BDA_KBTAIL]
        mov     si, di
        add     si, 2
        cmp     si, [es:BDA_KBSTOP]
        jb      .no_wrap
        mov     si, [es:BDA_KBSTART]
.no_wrap:
        cmp     si, [es:BDA_KBHEAD]
        je      .done                   ; full: the key is lost, as ever
        mov     [es:di], ax
        mov     [es:BDA_KBTAIL], si
        jmp     .done
.release:
        cmp     bl, 0x2A
        jne     .r1
        and     byte [es:BDA_SHIFT], ~0x02
.r1:    cmp     bl, 0x36
        jne     .r2
        and     byte [es:BDA_SHIFT], ~0x01
.r2:    cmp     bl, 0x1D
        jne     .r3
        and     byte [es:BDA_SHIFT], ~0x04
.r3:    cmp     bl, 0x38
        jne     .done
        and     byte [es:BDA_SHIFT], ~0x08
.done:
        mov     byte [kb_e0], 0
        cmp     al, 0xE0                ; ...unless that was the prefix itself
        jne     .ack
        mov     byte [kb_e0], 1
.ack:   mov     al, 0x20
        out     0x20, al
        popa
        pop     es
        pop     ds
        iret

; -----------------------------------------------------------------------------
; INT 16h: keys out of the buffer.
; -----------------------------------------------------------------------------
int16:
        push    bp
        mov     bp, sp                  ; [bp+6] = the flags to go back
        push    ds
        push    bx
        xor     bx, bx
        mov     ds, bx
        cmp     ah, 0x00
        je      .read
        cmp     ah, 0x10
        je      .read
        cmp     ah, 0x01
        je      .peek
        cmp     ah, 0x11
        je      .peek
        cmp     ah, 0x02
        je      .shift
        cmp     ah, 0x12
        je      .shift2
        cmp     ah, 0x05
        je      .push
        jmp     .out
.read:  sti
.wait:  mov     bx, [BDA_KBHEAD]
        cmp     bx, [BDA_KBTAIL]
        jne     .have
        hlt
        jmp     .wait
.have:  cli
        mov     ax, [bx]
        add     bx, 2
        cmp     bx, [BDA_KBSTOP]
        jb      .no_wrap
        mov     bx, [BDA_KBSTART]
.no_wrap:
        mov     [BDA_KBHEAD], bx
        jmp     .out
.peek:  mov     bx, [BDA_KBHEAD]
        cmp     bx, [BDA_KBTAIL]
        je      .empty
        mov     ax, [bx]
        and     word [bp + 6], ~0x40    ; ZF clear: a key waits
        jmp     .out
.empty: or      word [bp + 6], 0x40     ; ZF set
        jmp     .out
.shift: mov     al, [BDA_SHIFT]
        jmp     .out
.shift2:
        mov     al, [BDA_SHIFT]
        mov     ah, [BDA_SHIFT2]
        jmp     .out
.push:  ; CH:CL into the buffer, as if typed
        push    dx
        mov     bx, [BDA_KBTAIL]
        mov     dx, bx
        add     dx, 2
        cmp     dx, [BDA_KBSTOP]
        jb      .p_ok
        mov     dx, [BDA_KBSTART]
.p_ok:  cmp     dx, [BDA_KBHEAD]
        je      .p_full
        mov     [bx], cx
        mov     [BDA_KBTAIL], dx
        xor     al, al
        pop     dx
        jmp     .out
.p_full:
        mov     al, 1
        pop     dx
.out:   pop     bx
        pop     ds
        pop     bp
        iret

; -----------------------------------------------------------------------------
; INT 11h, 12h: what is fitted and how much of it
; -----------------------------------------------------------------------------
int11:  push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [BDA_EQUIP]
        pop     ds
        iret
int12:  push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [BDA_MEMKB]
        pop     ds
        iret

; -----------------------------------------------------------------------------
; INT 19h: start again.  The reset port on the south bridge does it on
;   everything this will ever run on; the keyboard controller's pulse is the
;   fallback, and after that, waiting.
; -----------------------------------------------------------------------------
int19:
        cli
        mov     al, 0x06
        out     0xCF9, al
        mov     al, 0xFE
        out     0x64, al
.halt:  hlt
        jmp     .halt

; -----------------------------------------------------------------------------
; INT 1Ah: the tick count and the real-time clock.  Times come back in BCD
;   because that is what a BIOS returns and what Ember converts from.
; -----------------------------------------------------------------------------
int1A:
        push    bp
        mov     bp, sp
        push    ds
        cmp     ah, 0x00
        je      .ticks
        cmp     ah, 0x01
        je      .set_ticks
        cmp     ah, 0x02
        je      .time
        cmp     ah, 0x04
        je      .date
        or      word [bp + 6], 1        ; anything else: unsupported
        jmp     .out
.ticks: xor     ax, ax
        mov     ds, ax
        mov     cx, [BDA_TICKS + 2]
        mov     dx, [BDA_TICKS]
        mov     al, [BDA_TICKOVF]
        mov     byte [BDA_TICKOVF], 0
        and     word [bp + 6], ~1
        jmp     .out
.set_ticks:
        xor     ax, ax
        mov     ds, ax
        mov     [BDA_TICKS + 2], cx
        mov     [BDA_TICKS], dx
        and     word [bp + 6], ~1
        jmp     .out
.time:  call    cmos_settle
        mov     al, 0x04                ; hours
        call    cmos_read
        mov     ch, al
        mov     al, 0x02                ; minutes
        call    cmos_read
        mov     cl, al
        mov     al, 0x00                ; seconds
        call    cmos_read
        mov     dh, al
        xor     dl, dl                  ; no daylight saving flag
        and     word [bp + 6], ~1
        jmp     .out
.date:  call    cmos_settle
        mov     al, 0x09                ; year
        call    cmos_read
        mov     cl, al
        mov     al, 0x32                ; century, where it is kept
        call    cmos_read
        mov     ch, al
        cmp     ch, 0x19
        jae     .century_ok
        mov     ch, 0x20                ; a chip that does not keep it
.century_ok:
        mov     al, 0x08                ; month
        call    cmos_read
        mov     dh, al
        mov     al, 0x07                ; day
        call    cmos_read
        mov     dl, al
        and     word [bp + 6], ~1
.out:   pop     ds
        pop     bp
        iret

; cmos_read: AL = register -> AL = value, in BCD whatever the chip's mode
cmos_read:
        push    bx
        or      al, 0x80                ; NMI off while the index is out
        out     0x70, al
        in      al, 0x71
        mov     bl, al
        mov     al, 0x8B                ; status B: bit 2 says binary
        out     0x70, al
        in      al, 0x71
        test    al, 0x04
        mov     al, bl
        jz      .bcd
        ; binary to BCD
        xor     ah, ah
        mov     bl, 10
        div     bl                      ; AL = tens, AH = units
        shl     al, 4
        or      al, ah
.bcd:   push    ax
        mov     al, 0x0D                ; the index somewhere harmless
        out     0x70, al
        pop     ax
        pop     bx
        ret

; cmos_settle: wait out an update in progress
cmos_settle:
        push    ax
        push    cx
        mov     cx, 0x4000
.wait:  mov     al, 0x8A
        out     0x70, al
        in      al, 0x71
        test    al, 0x80
        jz      .done
        loop    .wait
.done:  pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; INT 15h: memory, and the things a BIOS says no to
; -----------------------------------------------------------------------------
int15:
        push    bp
        mov     bp, sp
        push    ds
        push    cs
        pop     ds
        cmp     ax, 0xE820
        je      .e820
        cmp     ax, 0xE801
        je      .e801
        cmp     ah, 0x88
        je      .f88
        cmp     ax, 0x2401
        je      .a20_on
        cmp     ax, 0x2402
        je      .a20_query
        cmp     ax, 0x2403
        je      .a20_support
        cmp     ah, 0x86
        je      .wait
        cmp     ah, 0x4F
        je      .intercept
        cmp     ax, 0xE2B0
        je      .ember2
        jmp     .no
.ember2:
        ; Ember 2.0's own: "EMB2" in EAX, the page directory that maps the
        ; framebuffer's window in EBX, the window in ECX, its size in EDX.
        ; The kernel's 32-bit runtime asks once, and runs its programs with
        ; that directory loaded, so the address the VESA call gave them works.
        mov     eax, 0x32424D45
        mov     ebx, [pagedir_phys]
        mov     ecx, [h_fbwin]
        mov     edx, FBWIN_PDES << 22
        jmp     .yes
.e801:  ; AX = KB between 1 and 16 MB, BX = 64 KB blocks above 16 MB
        mov     eax, [h_ext_end]
        sub     eax, 0x100000
        shr     eax, 10
        cmp     eax, 0x3C00
        jbe     .e801_low
        mov     eax, 0x3C00
.e801_low:
        mov     cx, ax
        mov     ebx, [h_ext_end]
        cmp     ebx, 0x1000000
        jbe     .e801_none_above
        sub     ebx, 0x1000000
        shr     ebx, 16
        jmp     .e801_done
.e801_none_above:
        xor     ebx, ebx
.e801_done:
        mov     dx, bx
        jmp     .yes
.f88:   mov     eax, [h_ext_end]
        sub     eax, 0x100000
        shr     eax, 10
        cmp     eax, 0xFFFF
        jbe     .yes
        mov     ax, 0xFFFF
        jmp     .yes
.e820:  cmp     edx, 0x534D4150         ; "SMAP"
        jne     .no
        cmp     ebx, [h_e820_n]
        jae     .no
        push    si
        push    di
        push    cx
        mov     si, bx
        imul    si, 24
        add     si, h_e820
        cmp     ecx, 24
        jbe     .e820_len
        mov     ecx, 24
.e820_len:
        cmp     ecx, 20
        jae     .e820_copy
        mov     ecx, 20
.e820_copy:
        push    cx
        rep     movsb                   ; DS:SI (ours) -> ES:DI (theirs)
        pop     cx
        pop     ax                      ; the CX that was pushed
        pop     di
        pop     si
        mov     eax, 0x534D4150
        inc     ebx
        cmp     ebx, [h_e820_n]
        jb      .yes
        xor     ebx, ebx                ; the last one
        jmp     .yes
.a20_on:
        xor     ah, ah
        jmp     .yes
.a20_query:
        mov     al, 1
        xor     ah, ah
        jmp     .yes
.a20_support:
        mov     bx, 3
        xor     ah, ah
        jmp     .yes
.wait:  ; CX:DX microseconds, in 55 ms ticks, rounded up
        push    eax
        push    ebx
        push    ecx
        mov     eax, ecx
        shl     eax, 16
        mov     ax, dx
        xor     edx, edx
        mov     ebx, 54925
        div     ebx
        inc     eax
        mov     ecx, eax
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ebx, [BDA_TICKS]
        sti
.tick:  hlt
        mov     eax, [BDA_TICKS]
        sub     eax, ebx
        cmp     eax, ecx
        jb      .tick
        pop     ds
        pop     ecx
        pop     ebx
        pop     eax
        xor     ah, ah
        jmp     .yes
.intercept:
        or      word [bp + 6], 1        ; CF set: use the key as it is
        jmp     .out
.no:    mov     ah, 0x86
        or      word [bp + 6], 1
        jmp     .out
.yes:   and     word [bp + 6], ~1
.out:   pop     ds
        pop     bp
        iret

; -----------------------------------------------------------------------------
; INT 13h: the disk, which is a copy of the image in memory.  Drive 80h and
;   nothing else.  The extensions are what Ember uses; the old cylinder and
;   head calls are there because the boot sector asks about them first.
; -----------------------------------------------------------------------------
DISK_SPT        equ 63
DISK_HEADS      equ 16

int13:
        push    bp
        mov     bp, sp
        push    ds
        push    cs
        pop     ds
        cmp     dl, 0x80
        jne     .bad_drive
        cmp     ah, 0x00
        je      .ok
        cmp     ah, 0x01
        je      .status
        cmp     ah, 0x02
        je      .chs_read
        cmp     ah, 0x03
        je      .chs_write
        cmp     ah, 0x04
        je      .ok
        cmp     ah, 0x08
        je      .params
        cmp     ah, 0x15
        je      .type
        cmp     ah, 0x41
        je      .ext_check
        cmp     ah, 0x42
        je      .ext_read
        cmp     ah, 0x43
        je      .ext_write
        cmp     ah, 0x44
        je      .ok
        cmp     ah, 0x48
        je      .ext_params
        jmp     .unsupported
.status:
        mov     ah, [disk_status]
        jmp     .ok_keep
.ok:    xor     ah, ah
.ok_keep:
        mov     [disk_status], ah
        and     word [bp + 6], ~1
        jmp     .out
.bad_drive:
        mov     ah, 0x01
        jmp     .fail
.unsupported:
        mov     ah, 0x01
        jmp     .fail
.fail:  mov     [disk_status], ah
        or      word [bp + 6], 1
        jmp     .out
.out:   pop     ds
        pop     bp
        iret

.ext_check:
        cmp     bx, 0x55AA
        jne     .unsupported
        mov     bx, 0xAA55
        mov     ah, 0x30                ; version 3.0
        mov     cx, 0x0001              ; fixed disk access
        and     word [bp + 6], ~1
        jmp     .out

.params:
        push    eax
        call    disk_cylinders          ; EAX = cylinders, capped
        dec     eax
        mov     ch, al                  ; low eight bits
        mov     cl, ah
        shl     cl, 6
        or      cl, DISK_SPT
        pop     eax
        mov     dh, DISK_HEADS - 1
        mov     dl, 1                   ; one drive
        xor     ah, ah
        mov     bl, 0
        push    ax
        xor     ax, ax
        mov     es, ax
        mov     di, ax                  ; no parameter table
        pop     ax
        and     word [bp + 6], ~1
        jmp     .out

.type:  push    eax
        mov     eax, [h_rd_size]
        shr     eax, 9
        mov     dx, ax
        shr     eax, 16
        mov     cx, ax
        pop     eax
        mov     ah, 0x03                ; a fixed disk
        and     word [bp + 6], ~1
        jmp     .out

.ext_params:
        ; DS:SI -> a buffer with its size in the first word
        push    es
        push    di
        push    eax
        mov     ax, [bp - 2]            ; the caller's DS, saved above
        mov     es, ax
        mov     di, si
        cmp     word [es:di], 26
        jb      .ext_params_small
        mov     word [es:di], 26
        mov     word [es:di + 2], 0     ; no geometry to speak of
        call    disk_cylinders
        mov     [es:di + 4], eax
        mov     dword [es:di + 8], DISK_HEADS
        mov     dword [es:di + 12], DISK_SPT
        mov     eax, [h_rd_size]
        shr     eax, 9
        mov     [es:di + 16], eax
        mov     dword [es:di + 20], 0
        mov     word [es:di + 24], 512
        pop     eax
        pop     di
        pop     es
        xor     ah, ah
        and     word [bp + 6], ~1
        jmp     .out
.ext_params_small:
        pop     eax
        pop     di
        pop     es
        mov     ah, 0x01
        jmp     .fail

.chs_read:
        mov     byte [disk_op], 0
        jmp     .chs
.chs_write:
        mov     byte [disk_op], 1
.chs:   ; CH = cylinder low, CL = sector | cylinder high, DH = head, AL = count
        push    eax
        push    ebx
        push    ecx
        push    edx
        movzx   ebx, al                 ; the count
        mov     [xfer_count], ebx
        movzx   edx, dh                 ; head
        movzx   eax, ch
        mov     ah, cl
        shr     ah, 6                   ; cylinder 9:8 out of CL's top bits
        and     cx, 0x3F                ; the sector, from 1
        ; (cylinder * heads + head) * spt + sector - 1
        imul    eax, DISK_HEADS
        add     eax, edx
        imul    eax, DISK_SPT
        movzx   ecx, cl
        dec     ecx
        add     eax, ecx
        mov     [xfer_lba], eax
        mov     [xfer_lba + 4], dword 0
        movzx   eax, bx                 ; ES:BX, the caller's ES is on the
        mov     [xfer_off], ax          ;  stack under our frame
        mov     ax, es
        mov     [xfer_seg], ax
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        call    disk_transfer
        jc      .fail
        mov     al, [xfer_count]        ; sectors done
        jmp     .ok

.ext_read:
        mov     byte [disk_op], 0
        jmp     .ext
.ext_write:
        mov     byte [disk_op], 1
.ext:   ; DS:SI -> the disk address packet
        push    es
        push    eax
        push    si
        mov     ax, [bp - 2]
        mov     es, ax
        cmp     byte [es:si], 16
        jb      .dap_bad
        movzx   eax, word [es:si + 2]
        mov     [xfer_count], eax
        mov     ax, [es:si + 4]
        mov     [xfer_off], ax
        mov     ax, [es:si + 6]
        mov     [xfer_seg], ax
        mov     eax, [es:si + 8]
        mov     [xfer_lba], eax
        mov     eax, [es:si + 12]
        mov     [xfer_lba + 4], eax
        pop     si
        pop     eax
        pop     es
        call    disk_transfer
        jc      .fail
        jmp     .ok
.dap_bad:
        pop     si
        pop     eax
        pop     es
        mov     ah, 0x01
        jmp     .fail

; disk_cylinders: EAX = cylinders the image would have at 16 x 63, capped at
;   the 1024 the old interface can name
disk_cylinders:
        push    edx
        mov     eax, [h_rd_size]
        shr     eax, 9
        xor     edx, edx
        mov     ecx, DISK_HEADS * DISK_SPT
        div     ecx
        cmp     eax, 1024
        jbe     .capped
        mov     eax, 1024
.capped:
        pop     edx
        ret

; disk_transfer: xfer_* say what; CF=1 and AH = why if it cannot be done.
;   Everything is bounds checked against the image, then copied through
;   protected mode, because the image is above the megabyte.
disk_transfer:
        push    eax
        push    ebx
        push    ecx
        push    edx
        cmp     dword [xfer_lba + 4], 0
        jne     .beyond
        mov     eax, [xfer_lba]
        mov     ebx, [xfer_count]
        or      ebx, ebx
        jz      .nothing
        cmp     ebx, 256
        ja      .beyond
        mov     ecx, [h_rd_size]
        shr     ecx, 9                  ; sectors in the image
        cmp     eax, ecx
        jae     .beyond
        mov     edx, eax
        add     edx, ebx
        cmp     edx, ecx
        ja      .beyond
        shl     eax, 9
        add     eax, [h_rd_base]        ; the sector, in the image
        movzx   edx, word [xfer_seg]
        shl     edx, 4
        movzx   ecx, word [xfer_off]
        add     edx, ecx                ; the buffer, physical
        shl     ebx, 9                  ; bytes
        mov     [cp_len], ebx
        cmp     byte [disk_op], 0
        jne     .write
        mov     [cp_src], eax
        mov     [cp_dst], edx
        jmp     .go
.write: mov     [cp_src], edx
        mov     [cp_dst], eax
.go:    call    pm_copy
.nothing:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        xor     ah, ah
        clc
        ret
.beyond:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        mov     ah, 0x04                ; sector not found
        stc
        ret

; =============================================================================
;  INT 10h: the screen.  An 80 by 25 grid of characters and attributes kept
;  here, drawn onto the framebuffer a cell at a time.
; =============================================================================
int10:
        push    bp
        mov     bp, sp
        push    ds
        push    es
        push    cs
        pop     ds
        cmp     ah, 0x00
        je      .set_mode
        cmp     ah, 0x01
        je      .cursor_shape
        cmp     ah, 0x02
        je      .set_cursor
        cmp     ah, 0x03
        je      .get_cursor
        cmp     ah, 0x06
        je      .scroll_up
        cmp     ah, 0x07
        je      .scroll_down
        cmp     ah, 0x08
        je      .read_cell
        cmp     ah, 0x09
        je      .write_cells
        cmp     ah, 0x0A
        je      .write_chars
        cmp     ah, 0x0E
        je      .teletype
        cmp     ah, 0x0F
        je      .get_mode
        cmp     ah, 0x11
        je      .font
        cmp     ah, 0x12
        je      .ega_info
        cmp     ah, 0x13
        je      .write_string
        cmp     ah, 0x1A
        je      .combination
        cmp     ah, 0x4F
        je      .vesa
        jmp     .out                    ; palettes, pages, pens: nothing to do

.set_mode:
        ; Text modes are all the one mode here.  A VGA graphics mode is not
        ; something this machine has, and the honest thing is to leave the
        ; mode where it was - a caller that checks with 0Fh will see that it
        ; did not take, which is how a BIOS without that mode would answer.
        and     al, 0x7F
        cmp     al, 3
        ja      .out
        mov     byte [gfx_mode], 0      ; the console has the screen again
        call    screen_clear
        jmp     .out
.cursor_shape:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     [es:BDA_CURSHAPE], cx
        pop     es
        jmp     .out
.set_cursor:
        cmp     dh, ROWS
        jae     .out
        cmp     dl, COLS
        jae     .out
        call    cursor_move             ; DL, DH
        jmp     .out
.get_cursor:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     dx, [es:BDA_CURSOR]
        mov     cx, [es:BDA_CURSHAPE]
        pop     es
        jmp     .out
.scroll_up:
        mov     byte [scroll_dir], 0
        call    scroll_window
        jmp     .out
.scroll_down:
        mov     byte [scroll_dir], 1
        call    scroll_window
        jmp     .out
.read_cell:
        call    cursor_cell             ; BX -> the cell under the cursor
        mov     ax, [cells + bx]
        jmp     .out
.write_cells:
        mov     byte [attr_too], 1
        call    write_repeated
        jmp     .out
.write_chars:
        mov     byte [attr_too], 0
        call    write_repeated
        jmp     .out
.teletype:
        call    tty
        jmp     .out
.get_mode:
        mov     al, 3
        mov     ah, COLS
        xor     bh, bh
        jmp     .out
.font:  cmp     al, 0x30
        jne     .out
        ; ES:BP -> the font, CX = its height, DL = rows - 1
        mov     ax, cs
        mov     [bp - 4], ax            ; the caller's ES, saved above
        mov     word [bp], font         ; the caller's BP
        mov     cx, 16
        mov     dl, ROWS - 1
        jmp     .out
.ega_info:
        cmp     bl, 0x10
        jne     .out
        xor     bh, bh                  ; colour
        mov     bl, 3                   ; 256 KB
        xor     cx, cx
        jmp     .out
.write_string:
        call    write_string
        jmp     .out
.combination:
        cmp     al, 0
        jne     .out
        mov     al, 0x1A
        mov     bx, 0x0008              ; a colour VGA, on its own
        jmp     .out
.vesa:  ; VESA, with exactly one mode: the one the firmware left the panel
        ; in.  Its "physical" address is the window, which a program can use
        ; the moment it runs with paging on - and the kernel's 32-bit runtime
        ; does, once INT 15h E2B0h has told it where the page directory is.
        cmp     al, 0x00
        je      .vbe_info
        cmp     al, 0x01
        je      .vbe_mode
        cmp     al, 0x02
        je      .vbe_set
        cmp     al, 0x03
        je      .vbe_get
        mov     ax, 0x014F              ; anything else: AH != 0 is "failed"
        jmp     .out
.vbe_info:
        ; ES:DI -> 512 bytes, cleared by the caller
        mov     dword [es:di], "VESA"
        mov     word [es:di + 4], 0x0200
        mov     word [es:di + 6], vbe_oem
        mov     [es:di + 8], cs
        mov     word [es:di + 14], vbe_modes
        mov     [es:di + 16], cs
        mov     eax, [h_fb_h]
        imul    eax, [h_fb_pitch]
        shr     eax, 14                 ; bytes / 64 KB (4 bytes a pixel)
        cmp     eax, 0xFFFF
        jbe     .vbe_mem_ok
        mov     eax, 0xFFFF
.vbe_mem_ok:
        mov     [es:di + 18], ax
        mov     word [es:di + 22], vbe_oem
        mov     [es:di + 24], cs
        mov     word [es:di + 26], vbe_oem
        mov     [es:di + 28], cs
        mov     word [es:di + 30], vbe_oem
        mov     [es:di + 32], cs
        mov     ax, 0x004F
        jmp     .out
.vbe_mode:
        ; CX = the mode; ES:DI -> 256 bytes, cleared by the caller
        and     cx, 0x3FFF
        cmp     cx, VBE_MODE
        jne     .vbe_no_such
        mov     word [es:di], 0x009B    ; supported, colour, graphics, linear
        mov     eax, [h_fb_pitch]
        shl     eax, 2
        mov     [es:di + 16], ax        ; bytes per scan line
        mov     eax, [h_fb_w]
        mov     [es:di + 18], ax
        mov     eax, [h_fb_h]
        mov     [es:di + 20], ax
        mov     byte [es:di + 22], 8    ; a character cell, for what it is worth
        mov     byte [es:di + 23], 16
        mov     byte [es:di + 24], 1    ; one plane
        mov     byte [es:di + 25], 32   ; bits per pixel
        mov     byte [es:di + 26], 1    ; one bank
        mov     byte [es:di + 27], 6    ; direct colour
        mov     byte [es:di + 30], 1
        mov     byte [es:di + 31], 8    ; red: eight bits, at...
        mov     byte [es:di + 33], 8    ; green
        mov     byte [es:di + 34], 8    ;   ...at 8
        mov     byte [es:di + 35], 8    ; blue
        mov     byte [es:di + 37], 8    ; the spare byte
        mov     byte [es:di + 38], 24
        cmp     dword [h_fb_bgr], 0
        je      .vbe_rgb
        mov     byte [es:di + 32], 16   ; B G R: red is the third byte
        mov     byte [es:di + 36], 0
        jmp     .vbe_masks_done
.vbe_rgb:
        mov     byte [es:di + 32], 0    ; R G B: red is the first
        mov     byte [es:di + 36], 16
.vbe_masks_done:
        mov     eax, [fbwin_base]
        mov     [es:di + 40], eax       ; where to write: the window
        mov     eax, [h_fb_pitch]
        shl     eax, 2
        mov     [es:di + 50], ax        ; the same, the VBE 3 way
        mov     ax, 0x004F
        jmp     .out
.vbe_no_such:
        mov     ax, 0x014F
        jmp     .out
.vbe_set:
        mov     ax, bx
        and     ax, 0x3FFF
        cmp     ax, VBE_MODE
        je      .vbe_set_ours
        cmp     ax, 3                   ; text, by the VESA door
        jne     .vbe_no_such
        mov     byte [gfx_mode], 0
        call    screen_clear
        mov     ax, 0x004F
        jmp     .out
.vbe_set_ours:
        ; The panel is already in it.  All that changes is that the console
        ; keeps its hands off the framebuffer until text mode is asked for.
        mov     byte [gfx_mode], 1
        mov     ax, 0x004F
        jmp     .out
.vbe_get:
        mov     bx, 3
        cmp     byte [gfx_mode], 0
        je      .vbe_get_done
        mov     bx, VBE_MODE | 0x4000
.vbe_get_done:
        mov     ax, 0x004F
        jmp     .out
.out:   pop     es
        pop     ds
        pop     bp
        iret

; -----------------------------------------------------------------------------
; The cells and the cursor.  The cursor position lives in the BIOS data area,
; because programs look there; the cells live here.
; -----------------------------------------------------------------------------
; cursor_cell: BX = offset of the cursor's cell in cells (col*2 + row*160)
cursor_cell:
        push    es
        push    ax
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:BDA_CURSOR]
        call    cell_offset
        pop     ax
        pop     es
        ret

; cell_offset: AL = col, AH = row -> BX = offset
cell_offset:
        push    ax
        push    dx
        movzx   bx, ah
        imul    bx, COLS * 2
        movzx   dx, al
        shl     dx, 1
        add     bx, dx
        pop     dx
        pop     ax
        ret

; cursor_move: DL = col, DH = row.  The old cell is redrawn without the
;   cursor and the new one with it.
cursor_move:
        push    es
        push    ax
        push    dx
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:BDA_CURSOR]
        mov     [es:BDA_CURSOR], dx
        call    draw_one                ; AL, AH: the old
        mov     ax, dx
        call    draw_one                ; and the new
        pop     dx
        pop     ax
        pop     es
        ret

; draw_one: AL = col, AH = row: one cell onto the screen
draw_one:
        push    ax
        mov     [dr_col], al
        mov     [dr_row], ah
        mov     word [dr_n], 1
        call    pm_draw
        pop     ax
        ret

; draw_all: every cell
draw_all:
        mov     byte [dr_col], 0
        mov     byte [dr_row], 0
        mov     word [dr_n], COLS * ROWS
        call    pm_draw
        ret

; screen_clear: everything to spaces in light grey, cursor home
screen_clear:
        push    ax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di, cells
        mov     ax, 0x0720
        mov     cx, COLS * ROWS
        rep     stosw
        xor     ax, ax
        mov     es, ax
        mov     word [es:BDA_CURSOR], 0
        pop     es
        ; The text area is smaller than the panel, and a program that has
        ; just given the screen back leaves its picture in the margin
        ; around it.  A mode set is the moment to black the whole thing.
        mov     word [pm_routine], clear32
        call    pm_call
        call    draw_all
        pop     di
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; tty: AL onto the screen the way a BIOS does it: the bell is silent, back-
;   space goes back, return goes to the start of the line, line feed goes
;   down and scrolls at the bottom.  Everything else is a character, and the
;   attribute already in the cell is kept, as the real thing keeps it.
; -----------------------------------------------------------------------------
tty:
        push    es
        push    ax
        push    bx
        push    dx
        call    serial_putc
        push    ax
        xor     ax, ax
        mov     es, ax
        pop     ax
        mov     dx, [es:BDA_CURSOR]
        cmp     al, 7
        je      .done
        cmp     al, 8
        je      .bs
        cmp     al, 13
        je      .cr
        cmp     al, 10
        je      .lf
        ; a character
        push    ax
        mov     ax, dx
        call    cell_offset
        pop     ax
        mov     [cells + bx], al
        inc     dl
        cmp     dl, COLS
        jb      .move
        xor     dl, dl
        jmp     .lf_from_char
.bs:    or      dl, dl
        jz      .done
        dec     dl
        jmp     .move
.cr:    xor     dl, dl
        jmp     .move
.lf:
.lf_from_char:
        inc     dh
        cmp     dh, ROWS
        jb      .move
        dec     dh
        ; the bottom: everything up a line, the last line blank, the lot
        ; redrawn - which also draws the character just placed
        mov     [es:BDA_CURSOR], dx
        call    scroll_all_up
        jmp     .done
.move:  call    cursor_move             ; draws the old cell (with the new
.done:  pop     dx                      ;  character) and the new cursor
        pop     bx
        pop     ax
        pop     es
        ret

; scroll_all_up: one line, whole screen, attribute of the last line kept
scroll_all_up:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     si, cells + COLS * 2
        mov     di, cells
        mov     cx, COLS * (ROWS - 1)
        rep     movsw
        mov     ax, [cells + (COLS * (ROWS - 1) - 1) * 2]
        mov     al, ' '
        mov     cx, COLS
        rep     stosw
        pop     es
        call    draw_all
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; scroll_window: AL = lines (0: clear), BH = attribute for what comes in,
;   CH,CL = top-left row,col, DH,DL = bottom-right.  scroll_dir says which
;   way.  The window is moved in the cells and everything redrawn.
; -----------------------------------------------------------------------------
scroll_window:
        pusha
        push    es
        push    ds
        pop     es
        cmp     dh, ROWS
        jb      .r_ok
        mov     dh, ROWS - 1
.r_ok:  cmp     dl, COLS
        jb      .c_ok
        mov     dl, COLS - 1
.c_ok:  cmp     ch, dh
        ja      .done
        cmp     cl, dl
        ja      .done
        ; the window, in variables the helpers below read
        mov     [sw_attr], bh
        mov     [sw_top], ch
        mov     [sw_bottom], dh
        mov     [sw_left], cl
        mov     bl, dl
        sub     bl, cl
        inc     bl
        mov     [sw_width], bl          ; columns
        mov     bl, dh
        sub     bl, ch
        inc     bl                      ; rows in the window
        or      al, al
        jz      .clear_all
        cmp     al, bl
        jb      .some
.clear_all:
        mov     al, bl                  ; all of them: nothing survives
.some:  mov     [sw_lines], al
        mov     bh, bl
        sub     bh, al                  ; BH = rows that survive
        cmp     byte [scroll_dir], 0
        jne     .down
        ; up: from the top, row r takes row r + lines
        mov     dl, [sw_top]
.up:    or      bh, bh
        jz      .up_fill
        mov     dh, dl
        add     dh, [sw_lines]
        call    row_copy                ; DH -> DL
        inc     dl
        dec     bh
        jmp     .up
.up_fill:
        mov     al, [sw_lines]
.up_blank:
        or      al, al
        jz      .done
        call    row_blank               ; DL
        inc     dl
        dec     al
        jmp     .up_blank
.down:  ; down: from the bottom, row r takes row r - lines
        mov     dl, [sw_bottom]
.down_loop:
        or      bh, bh
        jz      .down_fill
        mov     dh, dl
        sub     dh, [sw_lines]
        call    row_copy
        dec     dl
        dec     bh
        jmp     .down_loop
.down_fill:
        mov     al, [sw_lines]
.down_blank:
        or      al, al
        jz      .done
        call    row_blank
        dec     dl
        dec     al
        jmp     .down_blank
.done:  pop     es
        popa
        call    draw_all
        ret

; row_addr: DL = row -> DI = offset of its sw_left column in cells
row_addr:
        movzx   di, dl
        imul    di, COLS * 2
        movzx   ax, byte [sw_left]
        shl     ax, 1
        add     di, ax
        add     di, cells
        ret

; row_copy: row DH's window columns into row DL's
row_copy:
        pusha
        call    row_addr
        push    di
        push    dx
        mov     dl, dh
        call    row_addr
        mov     si, di
        pop     dx
        pop     di
        movzx   cx, byte [sw_width]
        rep     movsw
        popa
        ret

; row_blank: row DL's window columns to spaces in sw_attr
row_blank:
        pusha
        call    row_addr
        movzx   cx, byte [sw_width]
        mov     ah, [sw_attr]
        mov     al, ' '
        rep     stosw
        popa
        ret

; -----------------------------------------------------------------------------
; write_repeated: AL = character, BL = attribute (if attr_too), CX = how many,
;   from the cursor, which does not move.
; -----------------------------------------------------------------------------
write_repeated:
        pusha
        push    es
        xor     dx, dx
        mov     es, dx
        mov     dx, [es:BDA_CURSOR]
        mov     [dr_col], dl
        mov     [dr_row], dh
        mov     [dr_n], cx
        push    ax
        mov     ax, dx
        call    cell_offset
        pop     ax
.each:  or      cx, cx
        jz      .drawn
        cmp     bx, COLS * ROWS * 2
        jae     .drawn
        mov     [cells + bx], al
        cmp     byte [attr_too], 0
        je      .no_attr
        mov     [cells + bx + 1], bl
.no_attr:
        add     bx, 2
        dec     cx
        jmp     .each
.drawn: call    pm_draw
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; write_string: ES:BP -> string, CX = length, DH,DL = where, BL = attribute,
;   AL = mode: bit 0 moves the cursor afterwards, bit 1 says attributes are
;   in the string too.  Done with the teletype for the control characters.
; -----------------------------------------------------------------------------
write_string:
        pusha
        push    es
        push    ds
        mov     [ws_mode], al
        mov     [ws_attr], bl
        ; the caller's ES:BP are in the frame
        mov     ax, [bp - 4]
        mov     [ws_seg], ax
        mov     ax, [bp]
        mov     [ws_off], ax
        push    dx
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:BDA_CURSOR]
        mov     [ws_saved], ax
        pop     dx
        call    cursor_move
.each:  or      cx, cx
        jz      .done
        mov     ds, [ws_seg]
        mov     si, [ws_off]
        lodsb
        mov     bl, [cs:ws_attr]
        test    byte [cs:ws_mode], 2
        jz      .no_attr
        mov     bl, [si]
        inc     si
        dec     cx
.no_attr:
        mov     [cs:ws_off], si
        push    cs
        pop     ds
        ; the attribute into the cell first, then the character by teletype
        push    ax
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:BDA_CURSOR]
        push    bx
        call    cell_offset
        pop     ax
        mov     [cells + bx + 1], al
        pop     es
        pop     ax
        call    tty
        dec     cx
        jmp     .each
.done:  push    cs
        pop     ds
        test    byte [ws_mode], 1
        jnz     .leave_cursor
        mov     dx, [ws_saved]
        call    cursor_move
.leave_cursor:
        pop     ds
        pop     es
        popa
        ret

; =============================================================================
;  The hop into protected mode and back.
; -----------------------------------------------------------------------------
;  Interrupts off, our own stack, our own GDT (a program may have loaded one
;  of its own since we were last here), then protected mode with paging on
;  and the framebuffer in its window.  The routine runs with CS and DS based
;  here, so the labels and variables mean what they mean everywhere else, and
;  ES flat, for the framebuffer and the disk.  Then everything back.
; =============================================================================
pm_copy:
        mov     word [pm_routine], copy32
        jmp     pm_call
pm_draw:
        cmp     byte [gfx_mode], 0      ; a program has the screen: the cells
        jne     .not_now                ;  are kept, but not drawn
        call    pm_draw_refresh         ; where the cursor is, for draw32
        mov     word [pm_routine], draw32
        jmp     pm_call
.not_now:
        ret

pm_call:
        pushf
        cli
        push    ds
        push    es
        push    fs
        push    gs
        pushad
        push    cs
        pop     ds
        mov     [pm_ss], ss
        mov     [pm_sp], sp
        mov     ax, cs
        mov     ss, ax
        mov     sp, pm_stack_top
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     dword SEL_SCODE:pm_land
[BITS 32]
pm_land:
        mov     ax, SEL_SDATA
        mov     ds, ax
        mov     ss, ax
        movzx   esp, sp
        mov     ax, SEL_DATA32
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        ; paging, with the framebuffer in its window
        mov     eax, [pagedir_phys]
        mov     cr3, eax
        mov     eax, cr4
        or      eax, 1 << 4             ; PSE: four-megabyte pages
        mov     cr4, eax
        mov     eax, cr0
        or      eax, 0x80000000
        mov     cr0, eax
        movzx   eax, word [pm_routine]
        call    eax
        mov     eax, cr0
        and     eax, 0x7FFFFFFF
        mov     cr0, eax
        jmp     SEL_CODE16:pm_back16
[BITS 16]
pm_back16:
        mov     ax, SEL_DATA16
        mov     ds, ax
        mov     ss, ax
        mov     eax, cr0
        and     al, 0xFE
        mov     cr0, eax
        jmp     far [rm_vec2]
rm_vec2:        dw pm_back, 0
pm_back:
        mov     ax, cs
        mov     ds, ax
        mov     ss, [pm_ss]
        mov     sp, [pm_sp]
        popad
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popf
        ret

; -----------------------------------------------------------------------------
; copy32: cp_len bytes from cp_src to cp_dst, both physical.  Nothing here
;   overlaps, so a straight copy.
; -----------------------------------------------------------------------------
[BITS 32]
copy32:
        push    ds
        mov     esi, [cp_src]
        mov     edi, [cp_dst]
        mov     ecx, [cp_len]
        mov     ax, SEL_DATA32
        mov     ds, ax
        mov     edx, ecx
        shr     ecx, 2
        rep     movsd
        mov     ecx, edx
        and     ecx, 3
        rep     movsb
        pop     ds
        ret

; -----------------------------------------------------------------------------
; draw32: dr_n cells from (dr_col, dr_row), each 8 by 16 glyph pixels
;   magnified [scale] times, onto the framebuffer through the window.  The
;   cell under the cursor gets its bottom two glyph rows in the foreground
;   colour.
;
;   For each glyph row one scan line's worth of pixels is built in linebuf and
;   then copied [scale] times, which is far fewer decisions than a pixel at a
;   time would be.
; -----------------------------------------------------------------------------
draw32:
        push    ebp
        movzx   ebx, byte [dr_col]
        movzx   ebp, byte [dr_row]
        movzx   ecx, word [dr_n]
.cell:  or      ecx, ecx
        jz      .done
        push    ecx
        ; the cell's contents
        mov     eax, ebp
        imul    eax, COLS
        add     eax, ebx
        movzx   edx, word [cells + eax * 2]     ; DL char, DH attribute
        ; its colours
        movzx   eax, dh
        and     eax, 0x0F
        mov     eax, [palette + eax * 4]
        mov     [fg], eax
        movzx   eax, dh
        shr     eax, 4
        mov     eax, [palette + eax * 4]
        mov     [bg], eax
        ; the cursor?
        mov     byte [under_cursor], 0
        movzx   eax, byte [cursor_col]
        cmp     ebx, eax
        jne     .not_cursor
        movzx   eax, byte [cursor_row]
        cmp     ebp, eax
        jne     .not_cursor
        mov     byte [under_cursor], 1
.not_cursor:
        ; where it goes: y = oy + row*16*s, x = ox + col*8*s
        movzx   eax, byte [scale]
        mov     esi, ebp
        imul    esi, eax
        shl     esi, 4                  ; row * 16 * s
        add     esi, [origin_y]
        imul    esi, [h_fb_pitch]       ; in pixels
        mov     edi, ebx
        imul    edi, eax
        shl     edi, 3                  ; col * 8 * s
        add     edi, [origin_x]
        add     esi, edi
        shl     esi, 2                  ; bytes
        add     esi, [fbwin_base]
        mov     [cell_ptr], esi
        ; the glyph
        movzx   esi, dl
        shl     esi, 4
        add     esi, font
        mov     [glyph_ptr], esi
        mov     byte [glyph_row], 0
.row:   ; build one line of 8*s pixels
        mov     esi, [glyph_ptr]
        movzx   eax, byte [glyph_row]
        mov     dl, [esi + eax]         ; the row's bits
        cmp     byte [under_cursor], 0
        je      .bits_as_is
        cmp     al, 14
        jb      .bits_as_is
        mov     dl, 0xFF                ; the cursor: a bar along the bottom
.bits_as_is:
        mov     edi, linebuf
        mov     ecx, 8
.bit:   mov     eax, [bg]
        test    dl, 0x80
        jz      .have_colour
        mov     eax, [fg]
.have_colour:
        push    ecx
        movzx   ecx, byte [scale]
.rep:   mov     [edi], eax
        add     edi, 4
        loop    .rep
        pop     ecx
        shl     dl, 1
        loop    .bit
        ; and copy it s times down the framebuffer
        mov     edi, [cell_ptr]
        movzx   ecx, byte [scale]
.line:  push    ecx
        mov     esi, linebuf
        movzx   ecx, byte [scale]
        shl     ecx, 3                  ; 8*s dwords
        push    edi
        rep     movsd                   ; DS:ESI (ours) -> ES:EDI (flat)
        pop     edi
        mov     eax, [h_fb_pitch]
        shl     eax, 2
        add     edi, eax
        pop     ecx
        loop    .line
        mov     [cell_ptr], edi         ; the next glyph row starts below
        inc     byte [glyph_row]
        cmp     byte [glyph_row], 16
        jb      .row
        ; the next cell
        pop     ecx
        dec     ecx
        inc     ebx
        cmp     ebx, COLS
        jb      .cell
        xor     ebx, ebx
        inc     ebp
        cmp     ebp, ROWS
        jb      .cell
.done:  pop     ebp
        ret

; clear32: the whole framebuffer to black, once, at the start
clear32:
        mov     edi, [fbwin_base]
        mov     eax, [h_fb_h]
        imul    eax, [h_fb_pitch]
        mov     ecx, eax
        xor     eax, eax
        rep     stosd
        ret
[BITS 16]

; -----------------------------------------------------------------------------
; screen_init: the page directory, the window, the scale, the palette, and a
;   black screen with the cursor at home.
; -----------------------------------------------------------------------------
screen_init:
        pusha
        push    es
        ; ---- the page directory: 4 MB pages, identity, but for the window ----
        movzx   eax, word [shim_seg]
        shl     eax, 4
        add     eax, pagedir
        mov     [pagedir_phys], eax
        mov     di, pagedir
        xor     eax, eax
        mov     cx, 1024
.pde:   mov     ebx, eax
        or      ebx, 0x83               ; present, writable, 4 MB
        mov     [di], ebx
        add     di, 4
        add     eax, 0x400000
        loop    .pde
        ; the window: the framebuffer, 4 MB aligned, bits 32-39 in 13-20
        mov     eax, [h_fb_base]
        mov     edx, [h_fb_base + 4]
        mov     ebx, eax
        and     ebx, 0x3FFFFF           ; the part below the alignment
        add     ebx, [h_fbwin]
        mov     [fbwin_base], ebx
        and     eax, 0xFFC00000
        and     edx, 0xFF
        shl     edx, 13
        or      eax, edx
        or      eax, 0x83
        ; ---- write-combining, or every pixel is a bus transaction ----
        ; What the framebuffer's writes cost is decided by its cache type,
        ; and that comes from the firmware's MTRRs for its real physical
        ; address - which nobody here can improve on, and which is usually
        ; "uncached".  The page attribute table can override that per page:
        ; entry 1 of the PAT, the one a page selects with its PWT bit, is
        ; redefined as write-combining, and the window's entries set PWT.
        ; Nothing else in this machine uses paging, so nothing else is
        ; affected.  On a desktop that copies 24 MB a frame, this is the
        ; difference between a lag and a machine.
        push    eax
        push    ecx
        push    edx
        mov     eax, 1
        cpuid
        test    edx, 1 << 16            ; the PAT is there (it always is)
        jz      .no_pat
        mov     ecx, 0x277              ; IA32_PAT
        rdmsr
        and     eax, 0xFFFF00FF         ; entry 1 (PWT): from write-through
        or      eax, 0x00000100         ;  to write-combining
        wrmsr
        mov     byte [pat_wc], 1
.no_pat:
        pop     edx
        pop     ecx
        pop     eax
        cmp     byte [pat_wc], 0
        je      .no_wc_bit
        or      eax, 0x08               ; PWT: PAT entry 1, write-combining
.no_wc_bit:
        mov     ebx, [h_fbwin]          ; its entries in the directory
        shr     ebx, 22
        shl     ebx, 2
        add     bx, pagedir
        mov     di, bx
        mov     cx, FBWIN_PDES
.win:   mov     [di], eax
        add     di, 4
        add     eax, 0x400000           ; (never carries: the base is aligned)
        loop    .win
        ; ---- the scale: the largest that fits 640 by 400 ----
        mov     eax, [h_fb_w]
        xor     edx, edx
        mov     ecx, 640
        div     ecx
        mov     ebx, eax
        mov     eax, [h_fb_h]
        xor     edx, edx
        mov     ecx, 400
        div     ecx
        cmp     eax, ebx
        jbe     .use_eax
        mov     eax, ebx
.use_eax:
        or      eax, eax
        jnz     .not_zero
        inc     eax
.not_zero:
        cmp     eax, SHIFT_MAX
        jbe     .scale_ok
        mov     eax, SHIFT_MAX
.scale_ok:
        mov     [scale], al
        ; centred
        movzx   ecx, al
        mov     eax, [h_fb_w]
        imul    ecx, 640
        sub     eax, ecx
        shr     eax, 1
        mov     [origin_x], eax
        movzx   ecx, byte [scale]
        mov     eax, [h_fb_h]
        imul    ecx, 400
        sub     eax, ecx
        shr     eax, 1
        mov     [origin_y], eax
        ; ---- the palette, in the framebuffer's byte order ----
        mov     si, cga
        mov     di, palette
        mov     cx, 16
.pal:   lodsb
        movzx   ebx, al                 ; red
        lodsb
        movzx   edx, al                 ; green
        lodsb
        movzx   eax, al                 ; blue
        shl     edx, 8
        cmp     dword [h_fb_bgr], 0
        je      .rgb
        shl     ebx, 16                 ; B | G<<8 | R<<16
        or      eax, ebx
        or      eax, edx
        jmp     .pal_store
.rgb:   shl     eax, 16                 ; R | G<<8 | B<<16
        or      eax, ebx
        or      eax, edx
.pal_store:
        mov     [di], eax
        add     di, 4
        loop    .pal
        ; ---- the cells blank, the screen black ----
        push    ds
        pop     es
        mov     di, cells
        mov     ax, 0x0720
        mov     cx, COLS * ROWS
        rep     stosw
        mov     word [pm_routine], clear32
        call    pm_call
        pop     es
        popa
        ret

; The draw routine wants the cursor position as bytes it can compare against
; its own loop counters; they are copied out of the BIOS data area whenever
; the cursor moves.  (cursor_move does it through pm_draw's caller.)
; To keep that simple, pm_draw refreshes them itself:
pm_draw_refresh:
        push    es
        push    ax
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:BDA_CURSOR]
        mov     [cursor_col], al
        mov     [cursor_row], ah
        pop     ax
        pop     es
        ret

; =============================================================================
;  The serial port, for a test bench that reads text rather than pictures.
;  Only if there is one: a port with nothing behind it reads back FFh, and
;  the scratch register would not hold what was put in it.
; =============================================================================
serial_init:
        mov     dx, 0x3FF
        mov     al, 0x5A
        out     dx, al
        in      al, dx
        cmp     al, 0x5A
        jne     .none
        mov     al, 0xA5
        out     dx, al
        in      al, dx
        cmp     al, 0xA5
        jne     .none
        mov     dx, 0x3FB
        mov     al, 0x80                ; the divisor latch
        out     dx, al
        mov     dx, 0x3F8
        mov     al, 1                   ; 115200
        out     dx, al
        inc     dx
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 0x03                ; 8 N 1
        out     dx, al
        mov     dx, 0x3F9
        xor     al, al                  ; no interrupts
        out     dx, al
        mov     byte [serial_on], 1
.none:  ret

serial_putc:
        cmp     byte [serial_on], 0
        je      .none
        push    ax
        push    dx
        push    cx
        mov     ah, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.wait:  in      al, dx
        test    al, 0x20
        jnz     .ready
        loop    .wait
.ready: mov     dx, 0x3F8
        mov     al, ah
        out     dx, al
        pop     cx
        pop     dx
        pop     ax
.none:  ret

; say: DS:SI text, by the teletype.  say_dec: EAX in decimal.
say:    push    ax
        push    bx
.next:  lodsb
        or      al, al
        jz      .done
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     .next
.done:  pop     bx
        pop     ax
        ret

say_dec:
        pushad
        mov     ecx, 10
        xor     ebx, ebx
.digit: xor     edx, edx
        div     ecx
        push    dx
        inc     ebx
        or      eax, eax
        jnz     .digit
.out:   pop     ax
        add     al, '0'
        mov     ah, 0x0E
        push    bx
        mov     bx, 7
        int     0x10
        pop     bx
        dec     ebx
        jnz     .out
        popad
        ret

; =============================================================================
;  Data
; =============================================================================
                align 8
gdt:            dq 0
gdt_code32:     dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
gdt_data32:     dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_code16:     dw 0xFFFF, 0x0000, 0x9A00, 0x0000
gdt_data16:     dw 0xFFFF, 0x0000, 0x9200, 0x0000
gdt_sdata:      dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_scode:      dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
gdt_end:
; Ten bytes, not six: LGDT in long mode reads a 64-bit base, and the same
; structure serves the 16-bit LGDT later, which reads the low four.
gdtr:           dw gdt_end - gdt - 1
                dq 0
idtr_real:      dw 0x03FF               ; the interrupt vector table, at nought
                dd 0

shim_seg:       dw 0
pagedir_phys:   dd 0
fbwin_base:     dd 0
origin_x:       dd 0
origin_y:       dd 0
scale:          db 1
serial_on:      db 0
gfx_mode:       db 0                    ; a program has the framebuffer
hb_count:       db 0                    ; ticks since the last heartbeat
pat_wc:         db 0                    ; PAT entry 1 is write-combining
kb_last:        db 0                    ; the last scan code that arrived
kb_e0:          db 0
kb_skip:        db 0
disk_status:    db 0
disk_op:        db 0
scroll_dir:     db 0
attr_too:       db 0
sw_attr:        db 0
sw_lines:       db 0
sw_top:         db 0
sw_bottom:      db 0
sw_left:        db 0
sw_width:       db 0
ws_mode:        db 0
ws_attr:        db 0
under_cursor:   db 0
glyph_row:      db 0
dr_col:         db 0
dr_row:         db 0
cursor_col:     db 0
cursor_row:     db 0
                align 2
kb_irqs:        dw 0                    ; keyboard interrupts, for the heartbeat
dr_n:           dw 0
ws_seg:         dw 0
ws_off:         dw 0
ws_saved:       dw 0
pm_routine:     dw 0
pm_ss:          dw 0
pm_sp:          dw 0
                align 4
xfer_count:     dd 0
xfer_lba:       dq 0
xfer_off:       dw 0
xfer_seg:       dw 0
cp_src:         dd 0
cp_dst:         dd 0
cp_len:         dd 0
fg:             dd 0
bg:             dd 0
cell_ptr:       dd 0
glyph_ptr:      dd 0
palette:        times 16 dd 0

cga:            db 0x00,0x00,0x00, 0x00,0x00,0xAA, 0x00,0xAA,0x00, 0x00,0xAA,0xAA
                db 0xAA,0x00,0x00, 0xAA,0x00,0xAA, 0xAA,0x55,0x00, 0xAA,0xAA,0xAA
                db 0x55,0x55,0x55, 0x55,0x55,0xFF, 0x55,0xFF,0x55, 0x55,0xFF,0xFF
                db 0xFF,0x55,0x55, 0xFF,0x55,0xFF, 0xFF,0xFF,0x55, 0xFF,0xFF,0xFF

vbe_oem:        db "Ember 2.0", 0
                align 2
vbe_modes:      dw VBE_MODE, 0xFFFF

msg_banner:     db 13, 10, "Ember 2.0 - UEFI stub.  Disk image ", 0
msg_banner2:    db " MB in memory, ", 0
msg_banner3:    db " KB conventional, ", 0
msg_banner4:    db " MB extended.", 13, 10, 13, 10, 0

; scan code set 1 to ASCII, plain and shifted
kb_normal:
        db 0, 27, "1234567890-=", 8, 9
        db "qwertyuiop[]", 13, 0, "asdfghjkl;'`", 0, "\zxcvbnm,./", 0, "*", 0, " ", 0
        db 0,0,0,0,0,0,0,0,0,0                  ; F1-F10
        db 0, 0                                 ; Num Lock, Scroll Lock
        db "789-456+1230."                      ; the keypad
        db 0, 0, 0, 0, 0                        ; 54h-58h: SysRq, -, -, F11, F12
kb_shifted:
        db 0, 27, "!@#$%^&*()_+", 8, 9
        db "QWERTYUIOP{}", 13, 0, 'ASDFGHJKL:"~', 0, "|ZXCVBNM<>?", 0, "*", 0, " ", 0
        db 0,0,0,0,0,0,0,0,0,0
        db 0, 0
        db "789-456+1230."
        db 0, 0, 0, 0, 0

                align 16
font:           incbin "shimfont.bin"           ; 256 glyphs, 8 by 16

                align 4
linebuf:        times 8 * SHIFT_MAX * 4 db 0    ; one scan line of a cell
cells:          times COLS * ROWS dw 0x0720

                align 16
stack:          times 2048 db 0
stack_top:
pm_stack:       times 2048 db 0
pm_stack_top:

                align 4096
pagedir:        times 1024 dd 0
shim_end:
