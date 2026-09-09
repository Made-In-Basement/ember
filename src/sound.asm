; =============================================================================
;  sound.asm - PC speaker music and an Intel HD Audio (HDA) WAV player
; -----------------------------------------------------------------------------
;  PC speaker: PIT channel 2 square waves; play_mml plays a note string.
;
;  HD Audio: the controller is found on the PCI bus, its memory-mapped
;  registers are reached through a 4 GB "unreal mode" GS segment, commands go
;  to the codec through the CORB/RIRB rings, and PCM data is streamed through
;  a two-half DMA ring buffer in conventional memory.  Every wait has a
;  timeout so a machine without HDA just reports "no sound hardware".
;
;  Physical memory used by the HDA driver (all below 1 MB, DMA-able):
;     0x40000  CORB (1 KB)      0x40400  RIRB (2 KB)     0x40C00  BDL
;     0x50000  PCM ring buffer, 2 x 32 KB (16-bit stereo frames)
; =============================================================================

CORB_OFF        equ 0x0000                      ; offsets within [hda_seg]
RIRB_OFF        equ 0x0400
BDL_OFF         equ 0x0C00
PCM_HALF        equ 0x8000                      ; bytes per half of [pcm_seg]
PCM_FRAMES      equ PCM_HALF / 4                ; 16-bit stereo frames per half

; controller registers
HDA_GCAP        equ 0x00
HDA_GCTL        equ 0x08
HDA_STATESTS    equ 0x0E
HDA_INTCTL      equ 0x20
HDA_CORBLBASE   equ 0x40
HDA_CORBUBASE   equ 0x44
HDA_CORBWP      equ 0x48
HDA_CORBRP      equ 0x4A
HDA_CORBCTL     equ 0x4C
HDA_CORBSIZE    equ 0x4E
HDA_RIRBLBASE   equ 0x50
HDA_RIRBUBASE   equ 0x54
HDA_RIRBWP      equ 0x58
HDA_RINTCNT     equ 0x5A
HDA_RIRBCTL     equ 0x5C
HDA_RIRBSTS     equ 0x5D
HDA_RIRBSIZE    equ 0x5E
HDA_DPLBASE     equ 0x70
; stream descriptor (relative)
SD_CTL          equ 0x00
SD_CTL2         equ 0x02
SD_STS          equ 0x03
SD_LPIB         equ 0x04
SD_CBL          equ 0x08
SD_LVI          equ 0x0C
SD_FMT          equ 0x12
SD_BDPL         equ 0x18
SD_BDPU         equ 0x1C

; =============================================================================
;  PC speaker
; =============================================================================
; spk_tone: AX = frequency in Hz (0 = off)
spk_tone:
        pusha
        call    hda_beep_tone                   ; codec beep too, when present
        or      ax, ax
        jz      .off
        mov     bx, ax
        mov     al, 0xB6                        ; channel 2, square wave
        out     0x43, al
        mov     dx, 0x0012
        mov     ax, 0x34DC                      ; DX:AX = 1193182
        div     bx                              ; / frequency
        out     0x42, al
        mov     al, ah
        out     0x42, al
        in      al, 0x61
        or      al, 0x03
        out     0x61, al
        popa
        ret
.off:   in      al, 0x61
        and     al, 0xFC
        out     0x61, al
        popa
        ret

spk_off:
        push    ax
        xor     ax, ax
        call    hda_beep_tone
        in      al, 0x61
        and     al, 0xFC
        out     0x61, al
        pop     ax
        ret

; -----------------------------------------------------------------------------
; play_mml: DS:SI = note string.  Subset of the classic PLAY language:
;   A-G notes (with # or + for sharp, - for flat), R or P = rest,
;   Ln = length (1..64, "." dots allowed after a note), On = octave 0..7,
;   > < = octave up/down, Tn = tempo, spaces ignored.  Esc stops.
; -----------------------------------------------------------------------------
play_mml:
        pusha
        call    hda_init                        ; codec beep generator, if any
        jc      .no_codec
        call    enter_unreal
        call    hda_setup_path
.no_codec:
        mov     byte [mml_octave], 4
        mov     byte [mml_length], 4
        mov     byte [mml_tempo], 120
.next:  mov     al, [si]
        or      al, al
        jz      .done
        inc     si
        call    upcase
        cmp     al, ' '
        je      .next
        cmp     al, 'O'
        je      .octave
        cmp     al, '>'
        je      .up
        cmp     al, '<'
        je      .down
        cmp     al, 'L'
        je      .length
        cmp     al, 'T'
        je      .tempo
        cmp     al, 'R'
        je      .rest
        cmp     al, 'P'
        je      .rest
        cmp     al, 'A'
        jb      .next
        cmp     al, 'G'
        ja      .next
        ; note: index into the semitone table
        sub     al, 'A'
        mov     bx, note_index
        xlat                                    ; AL = semitone 0..11
        mov     bl, al
        mov     al, [si]
        cmp     al, '#'
        je      .sharp
        cmp     al, '+'
        je      .sharp
        cmp     al, '-'
        jne     .note_ready
        dec     bl
        inc     si
        jmp     .note_ready
.sharp: inc     bl
        inc     si
.note_ready:
        ; frequency = table[semitone] scaled by octave (table is octave 4)
        push    bx
        call    mml_read_number                 ; optional length after the note
        pop     bx
        mov     dl, al                          ; DL = length or 0
        push    dx
        movzx   bx, bl
        cmp     bl, 12
        jb      .in_range
        xor     bl, bl
.in_range:
        shl     bx, 1
        mov     ax, [note_freq+bx]
        mov     cl, [mml_octave]
        cmp     cl, 4
        je      .have_freq
        jb      .lower
        sub     cl, 4
        shl     ax, cl
        jmp     .have_freq
.lower: mov     ch, 4
        sub     ch, cl
        mov     cl, ch
        shr     ax, cl
.have_freq:
        pop     dx
        call    spk_tone
        call    mml_note_ticks                  ; CX = ticks (uses DL, dots at SI)
        call    mml_wait
        call    spk_off
        jc      .done                           ; Esc pressed
        mov     cx, 1
        call    delay_ticks                     ; short gap between notes
        jmp     .next
.rest:  call    mml_read_number
        mov     dl, al
        call    mml_note_ticks
        call    mml_wait
        jc      .done
        jmp     .next
.octave:
        call    mml_read_number
        cmp     al, 7
        ja      .next
        mov     [mml_octave], al
        jmp     .next
.up:    cmp     byte [mml_octave], 7
        jae     .next
        inc     byte [mml_octave]
        jmp     .next
.down:  cmp     byte [mml_octave], 0
        je      .next
        dec     byte [mml_octave]
        jmp     .next
.length:
        call    mml_read_number
        or      al, al
        jz      .next
        mov     [mml_length], al
        jmp     .next
.tempo: call    mml_read_number
        cmp     al, 32
        jb      .next
        mov     [mml_tempo], al
        jmp     .next
.done:  call    spk_off
        popa
        ret

; mml_read_number: digits at DS:SI -> AL (0 if none), SI advanced
mml_read_number:
        push    bx
        xor     bx, bx
.digit: mov     al, [si]
        cmp     al, '0'
        jb      .done
        cmp     al, '9'
        ja      .done
        sub     al, '0'
        mov     ah, bl
        mov     bl, 10
        push    ax
        mov     al, ah
        mul     bl
        mov     bl, al
        pop     ax
        add     bl, al
        inc     si
        jmp     .digit
.done:  mov     al, bl
        pop     bx
        ret

; mml_note_ticks: DL = explicit length (0 = default) -> CX = ticks; handles dots
mml_note_ticks:
        push    ax
        push    dx
        or      dl, dl
        jnz     .have_len
        mov     dl, [mml_length]
.have_len:
        ; ticks = 4368 / (length * tempo)   (a quarter note at T120 = 9 ticks)
        movzx   ax, dl
        mov     cl, [mml_tempo]
        xor     ch, ch
        mul     cx
        mov     cx, ax
        mov     ax, 4368
        xor     dx, dx
        div     cx
        mov     cx, ax
        or      cx, cx
        jnz     .dots
        inc     cx
.dots:  cmp     byte [si], '.'
        jne     .done
        inc     si
        mov     ax, cx
        shr     ax, 1
        add     cx, ax
        jmp     .dots
.done:  pop     dx
        pop     ax
        ret

; mml_wait: wait CX ticks, CF=1 if Esc was pressed
mml_wait:
        push    ax
        push    cx
.tick:  call    kbhit
        jz      .no_key
        call    getkey
        cmp     al, 27
        je      .stop
.no_key:
        push    cx
        mov     cx, 1
        call    delay_ticks
        pop     cx
        loop    .tick
        pop     cx
        pop     ax
        clc
        ret
.stop:  pop     cx
        pop     ax
        stc
        ret

note_index:     db 9, 11, 0, 2, 4, 5, 7          ; A B C D E F G -> semitone
note_freq:      dw 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494

; =============================================================================
;  Unreal mode: give GS a 4 GB limit so MMIO above 1 MB can be reached
; =============================================================================
enter_unreal:
        pushad
        push    ds
        cli
        ; A20 (harmless if already on)
        mov     ax, 0x2401
        int     0x15
        in      al, 0x92
        test    al, 0x02
        jnz     .a20_ok
        or      al, 0x02
        and     al, 0xFE
        out     0x92, al
.a20_ok:
        ; GDT base = linear address of gdt
        mov     ax, ds
        movzx   eax, ax
        shl     eax, 4
        add     eax, gdt
        mov     [gdt_ptr+2], eax
        lgdt    [gdt_ptr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     short .pm
.pm:    mov     bx, 0x08
        mov     gs, bx
        and     al, 0xFE
        mov     cr0, eax
        jmp     short .rm
.rm:    xor     ax, ax
        mov     gs, ax                          ; base 0, limit stays 4 GB
        sti
        pop     ds
        popad
        ret

gdt:    dq 0
        dw 0xFFFF, 0x0000
        db 0x00, 0x92, 0xCF, 0x00
gdt_end:
gdt_ptr:
        dw gdt_end - gdt - 1
        dd 0

; ---- MMIO helpers: EBX = register offset ------------------------------------
hda_rd32:
        push    ebx
        add     ebx, [hda_base]
        mov     eax, [gs:ebx]
        pop     ebx
        ret
hda_wr32:
        push    ebx
        add     ebx, [hda_base]
        mov     [gs:ebx], eax
        pop     ebx
        ret
hda_rd16:
        push    ebx
        add     ebx, [hda_base]
        mov     ax, [gs:ebx]
        pop     ebx
        ret
hda_wr16:
        push    ebx
        add     ebx, [hda_base]
        mov     [gs:ebx], ax
        pop     ebx
        ret
hda_rd8:
        push    ebx
        add     ebx, [hda_base]
        mov     al, [gs:ebx]
        pop     ebx
        ret
hda_wr8:
        push    ebx
        add     ebx, [hda_base]
        mov     [gs:ebx], al
        pop     ebx
        ret

; io_delay_ms: CX = milliseconds (rough, via port 80h writes)
io_delay_ms:
        push    ax
        push    cx
        push    dx
.ms:    mov     dx, 1000
.us:    out     0x80, al
        dec     dx
        jnz     .us
        loop    .ms
        pop     dx
        pop     cx
        pop     ax
        ret

; =============================================================================
;  PCI: find the HDA controller (class 04h, subclass 03h)
; =============================================================================
; pci_read32: EBX = config address (bus/dev/func/reg) -> EAX
pci_read32:
        push    dx
        mov     eax, ebx
        or      eax, 0x80000000
        mov     dx, 0xCF8
        out     dx, eax
        mov     dx, 0xCFC
        in      eax, dx
        pop     dx
        ret
pci_write32:
        push    dx
        push    eax
        mov     eax, ebx
        or      eax, 0x80000000
        mov     dx, 0xCF8
        out     dx, eax
        pop     eax
        mov     dx, 0xCFC
        out     dx, eax
        pop     dx
        ret

; hda_find: locate the CL-th (0-based) HDA controller.  CF=1 if none.
;   Sets hda_base, hda_pci.  (Laptops often have two: the display's HDMI
;   audio controller and the chipset one that drives the speakers.)
hda_find:
        pushad
        xor     ebx, ebx                        ; bus 0, dev 0, func 0
.scan:  mov     eax, ebx
        add     eax, 0x08                       ; class register
        push    ebx
        mov     ebx, eax
        call    pci_read32
        pop     ebx
        cmp     eax, 0xFFFFFFFF
        je      .next
        shr     eax, 16
        cmp     ax, 0x0403
        jne     .next
        or      cl, cl
        jz      .found
        dec     cl
.next:  add     ebx, 0x100                      ; next function
        cmp     ebx, 0x00080000                 ; buses 0..7
        jb      .scan
        popad
        stc
        ret
.found: mov     [hda_pci], ebx
        ; BAR0
        push    ebx
        add     ebx, 0x10
        call    pci_read32
        pop     ebx
        and     eax, 0xFFFFFFF0
        mov     [hda_base], eax
        ; enable memory space + bus master
        push    ebx
        add     ebx, 0x04
        call    pci_read32
        or      eax, 0x06
        call    pci_write32
        pop     ebx
        ; traffic class 0 (TCSEL, offset 44h) so DMA is not marked "isochronous"
        push    ebx
        add     ebx, 0x44
        call    pci_read32
        and     eax, 0xFFFFFFF8
        call    pci_write32
        pop     ebx
        ; Intel controllers: clear the NoSnoop bit (DEVC, offset 78h, bit 11) so
        ; the controller's DMA goes through the CPU caches
        push    ebx
        call    pci_read32
        cmp     ax, 0x8086
        jne     .not_intel
        add     ebx, 0x78
        call    pci_read32
        and     eax, 0xFFFFF7FF
        call    pci_write32
.not_intel:
        pop     ebx
        popad
        clc
        ret

; =============================================================================
;  HDA controller and codec initialisation
; =============================================================================
; hda_init: CF=1 on failure (reason in hda_status).  Idempotent.
hda_init:
        pushad
        push    es
        cmp     byte [hda_ready], 0
        jne     .ok
        mov     byte [hda_index], 0
.try_controller:
        mov     byte [hda_status], 1            ; no controller
        mov     cl, [hda_index]
        call    hda_find
        jc      .fail
        call    enter_unreal
        ; zero the command/response rings (RAM survives warm reboots)
        mov     es, [hda_seg]
        xor     di, di
        xor     eax, eax
        mov     cx, 0x400                       ; 4 KB: CORB, RIRB, BDL
        rep     stosd
        wbinvd
        mov     byte [hda_status], 2            ; controller reset failed
        mov     eax, [hda_base]
        mov     si, dbg_base
        call    dbg
        ; ---- reset ----
        mov     ebx, HDA_GCTL
        xor     eax, eax
        call    hda_wr32
        mov     cx, 100
.wait_low:
        call    hda_rd32
        test    al, 1
        jz      .is_low
        push    cx
        mov     cx, 1
        call    io_delay_ms
        pop     cx
        loop    .wait_low
        jmp     .fail
.is_low:
        mov     cx, 2
        call    io_delay_ms
        mov     eax, 1
        call    hda_wr32
        mov     cx, 100
.wait_high:
        call    hda_rd32
        test    al, 1
        jnz     .is_high
        push    cx
        mov     cx, 1
        call    io_delay_ms
        pop     cx
        loop    .wait_high
        jmp     .fail
.is_high:
        mov     cx, 25                          ; let the codecs report in
        call    io_delay_ms
        mov     byte [hda_status], 3            ; no codec
        mov     ebx, HDA_STATESTS
        call    hda_rd16
        and     eax, 0x7FFF
        mov     si, dbg_statests
        call    dbg
        test    eax, eax
        jz      .fail
        xor     cl, cl
.find_codec:
        test    ax, 1
        jnz     .codec_found
        shr     ax, 1
        inc     cl
        jmp     .find_codec
.codec_found:
        mov     [hda_codec], cl
        ; ---- no interrupts, no DMA position buffer ----
        mov     ebx, HDA_INTCTL
        xor     eax, eax
        call    hda_wr32
        mov     ebx, HDA_DPLBASE
        call    hda_wr32
        ; ---- CORB ----
        mov     ebx, HDA_CORBCTL
        xor     al, al
        call    hda_wr8
        mov     ebx, HDA_CORBSIZE
        mov     al, 2                           ; 256 entries
        call    hda_wr8
        mov     ebx, HDA_CORBLBASE
        movzx   eax, word [hda_seg]
        shl     eax, 4
        add     eax, CORB_OFF
        call    hda_wr32
        mov     ebx, HDA_CORBUBASE
        xor     eax, eax
        call    hda_wr32
        mov     ebx, HDA_CORBRP
        mov     ax, 0x8000                      ; reset read pointer
        call    hda_wr16
        mov     cx, 5
        call    io_delay_ms
        xor     ax, ax
        call    hda_wr16
        mov     ebx, HDA_CORBWP
        call    hda_wr16
        mov     word [corb_wp], 0
        ; ---- RIRB ----
        mov     ebx, HDA_RIRBCTL
        xor     al, al
        call    hda_wr8
        mov     ebx, HDA_RIRBSIZE
        mov     al, 2
        call    hda_wr8
        mov     ebx, HDA_RIRBLBASE
        movzx   eax, word [hda_seg]
        shl     eax, 4
        add     eax, RIRB_OFF
        call    hda_wr32
        mov     ebx, HDA_RIRBUBASE
        xor     eax, eax
        call    hda_wr32
        mov     ebx, HDA_RIRBWP
        mov     ax, 0x8000
        call    hda_wr16
        mov     ebx, HDA_RINTCNT
        mov     ax, 1
        call    hda_wr16
        mov     word [rirb_rp], 0
        ; run both rings
        mov     ebx, HDA_CORBCTL
        mov     al, 0x02
        call    hda_wr8
        mov     ebx, HDA_RIRBCTL
        mov     al, 0x03                        ; DMA run + response "interrupt"
        call    hda_wr8                         ; (GIE is off, so it only sets a
        mov     cx, 2                           ;  status bit we acknowledge)
        call    io_delay_ms
        ; ---- codec: find the output path ----
        mov     byte [hda_status], 4            ; codec not answering / no analog pin
        call    hda_find_path
        jnc     .path_ok
        ; this controller has no usable output (e.g. HDMI only): try the next
        inc     byte [hda_index]
        cmp     byte [hda_index], 4
        jb      .try_controller
        jmp     .fail
.path_ok:
        mov     byte [hda_status], 0
        mov     byte [hda_ready], 1
.ok:    pop     es
        popad
        clc
        ret
.fail:  pop     es
        popad
        stc
        ret

; -----------------------------------------------------------------------------
; hda_cmd: send a verb.  AL = node id, EDX = verb (20 bits: verb<<8|payload
;   or verb<<16|payload16).  Returns EAX = response, CF=1 on timeout.
; -----------------------------------------------------------------------------
hda_cmd:
        push    ebx
        push    ecx
        push    edx
        push    es
        ; build the command word: cad<<28 | nid<<20 | verb
        movzx   ecx, al
        shl     ecx, 20
        movzx   eax, byte [hda_codec]
        shl     eax, 28
        or      eax, ecx
        and     edx, 0x000FFFFF
        or      eax, edx
        ; write into the CORB
        mov     es, [hda_seg]
        mov     bx, [corb_wp]
        inc     bx
        and     bx, 0xFF
        mov     [corb_wp], bx
        shl     bx, 2
        mov     [es:CORB_OFF+bx], eax
        wbinvd                                  ; push the entry out to RAM
        mov     ebx, HDA_RIRBWP
        call    hda_rd16
        and     ax, 0xFF
        mov     [rirb_rp], ax                   ; responses so far
        mov     ax, [corb_wp]
        mov     ebx, HDA_CORBWP
        call    hda_wr16
        ; wait for the response
        mov     cx, 1000                        ; up to ~1 s
.wait:  mov     ebx, HDA_RIRBWP
        call    hda_rd16
        and     ax, 0xFF
        cmp     ax, [rirb_rp]
        jne     .got
        push    cx
        mov     cx, 1
        call    io_delay_ms
        pop     cx
        loop    .wait
        mov     bx, [corb_wp]
        shl     bx, 2
        mov     eax, [es:CORB_OFF+bx]
        mov     si, dbg_cmd
        call    dbg
        mov     ebx, HDA_CORBWP
        call    hda_rd32                        ; CORBWP | CORBRP<<16
        mov     si, dbg_corbptrs
        call    dbg
        mov     ebx, HDA_RIRBWP
        call    hda_rd16
        movzx   eax, ax
        mov     si, dbg_rirbwp
        call    dbg
        mov     ebx, HDA_RIRBSTS
        call    hda_rd8
        movzx   eax, al
        mov     si, dbg_rirbsts
        call    dbg
        pop     es
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret
.got:   mov     cx, 1
        call    io_delay_ms                     ; let the DMA write land
        wbinvd                                  ; drop any stale cached copy
        mov     bx, ax                          ; read the newest entry (at WP)
        mov     [rirb_rp], bx
        shl     bx, 3
        mov     eax, [es:RIRB_OFF+bx]
        ; acknowledge the response (the controller stalls the CORB after
        ; RINTCNT responses until the status bit is cleared)
        push    eax
        mov     ebx, HDA_RIRBSTS
        mov     al, 0x05
        call    hda_wr8
        pop     eax
        push    eax
        mov     si, dbg_cmd
        mov     bx, [corb_wp]
        shl     bx, 2
        mov     eax, [es:CORB_OFF+bx]
        call    dbg
        pop     eax
        mov     si, dbg_resp
        call    dbg
        pop     es
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret

; dbg: when [snd_debug] is set, print the string at SI and EAX in hex,
;   pausing for a key every screenful so the trace can be read
dbg:
        call    log_line                        ; always into the boot log
        cmp     byte [snd_debug], 0
        je      .done
        call    puts
        call    print_hex32
        call    crlf
        inc     byte [dbg_lines]
        cmp     byte [dbg_lines], 22
        jb      .done
        mov     byte [dbg_lines], 0
        push    si
        mov     si, msg_more
        call    puts
        pop     si
        call    getkey
        call    crlf
.done:  ret

; hda_param: AL = node, CL = parameter id -> EAX
hda_param:
        push    edx
        movzx   edx, cl
        or      edx, 0xF0000
        call    hda_cmd
        pop     edx
        ret

; hda_widget_type: AL = node -> AL = widget type (0 = output, 2 mixer, 3 sel, 4 pin)
hda_widget_type:
        push    eax
        mov     cl, 0x09
        call    hda_param
        shr     eax, 20
        and     al, 0x0F
        mov     [wtype_tmp], al
        pop     eax
        mov     al, [wtype_tmp]
        ret

; hda_find_path: locate AFG, a speaker/headphone/line-out pin and its DAC.
;   Sets hda_afg, hda_pin, hda_dac, hda_mixer (0 if none).  CF=1 on failure
hda_find_path:
        pushad
        mov     byte [hda_afg], 0
        mov     byte [hda_pin], 0
        mov     byte [hda_dac], 0
        mov     byte [hda_mixer], 0
        mov     byte [hda_beep], 0
        mov     byte [hda_inmix], 0
        mov     byte [hda_pin_rank], 0
        ; root node 0: subordinate node count
        xor     al, al
        mov     cl, 0x04
        call    hda_param
        mov     si, dbg_root
        jc      .dbg_fail
        call    dbg
        mov     edx, eax
        shr     edx, 16
        and     dl, 0xFF                        ; DL = first function group
        and     al, 0xFF                        ; AL = count
        mov     dh, al
        or      dh, dh
        jz      .fail
.fg:    mov     al, dl
        mov     cl, 0x05                        ; function group type
        call    hda_param
        and     al, 0x7F
        cmp     al, 0x01                        ; audio function group
        je      .afg
        inc     dl
        dec     dh
        jnz     .fg
        jmp     .fail
.afg:   mov     [hda_afg], dl
        mov     al, dl
        mov     edx, 0x70500                    ; power state D0
        call    hda_cmd
        mov     al, [hda_afg]
        mov     cl, 0x04
        call    hda_param
        mov     si, dbg_afg
        call    dbg
        mov     edx, eax
        shr     edx, 16
        and     dl, 0xFF                        ; DL = first widget
        and     al, 0xFF
        mov     dh, al                          ; DH = widget count
        or      dh, dh
        jz      .fail
        ; ---- scan widgets for the best output pin ----
.widget:
        mov     al, dl
        call    hda_widget_type
        cmp     al, 7                           ; beep generator: remember it
        jne     .not_beep
        mov     [hda_beep], dl
.not_beep:
        cmp     al, 4
        jne     .next_widget
        mov     al, dl
        push    dx
        mov     edx, 0xF1C00                    ; configuration default
        call    hda_cmd
        pop     dx
        jc      .next_widget
        push    dx
        mov     si, dbg_pin
        push    eax
        movzx   eax, dl
        call    dbg
        pop     eax
        mov     si, dbg_cfg
        call    dbg
        pop     dx
        ; port connectivity (bits 30-31): 1 = no physical connection
        mov     ecx, eax
        shr     ecx, 30
        cmp     cl, 1
        je      .next_widget
        shr     eax, 20
        and     al, 0x0F                        ; default device
        mov     cl, 3                           ; rank: speaker best
        cmp     al, 1
        je      .rank
        mov     cl, 2
        cmp     al, 2                           ; headphone
        je      .rank
        mov     cl, 1
        cmp     al, 0                           ; line out
        jne     .next_widget
.rank:  cmp     cl, [hda_pin_rank]
        jbe     .next_widget
        ; must have an output-capable pin (pin caps bit 4)
        mov     al, dl
        push    cx
        mov     cl, 0x0C
        call    hda_param
        pop     cx
        test    al, 0x10
        jz      .next_widget
        mov     [hda_pin_rank], cl
        mov     [hda_pin], dl
.next_widget:
        inc     dl
        dec     dh
        jnz     .widget
        movzx   eax, byte [hda_pin]
        mov     si, dbg_chosen
        call    dbg
        cmp     byte [hda_pin], 0
        je      .fail
        ; ---- follow the pin's connection list to a DAC ----
        mov     al, [hda_pin]
        call    hda_dac_from
        mov     si, dbg_nodac
        jc      .dbg_fail
        movzx   eax, byte [hda_dac]
        mov     si, dbg_dac
        call    dbg
        popad
        clc
        ret
.dbg_fail:
        xor     eax, eax
        call    dbg
.fail:  popad
        stc
        ret

; hda_dac_from: AL = widget.  Finds an Audio Output in its connection list
;   (directly or through one mixer/selector).  Sets hda_dac / hda_mixer and
;   the connection selects.  CF=1 if none.
hda_dac_from:
        pushad
        mov     [dac_search_node], al
        mov     cl, 0x0E                        ; connection list length
        call    hda_param
        jc      .fail
        and     al, 0x7F
        mov     [conn_count], al
        or      al, al
        jz      .fail
        xor     dl, dl                          ; DL = index
.entry: push    dx
        movzx   edx, dl
        and     dl, 0xFC                        ; entries come 4 at a time
        or      edx, 0xF0200                    ; get connection list entry
        mov     al, [dac_search_node]
        call    hda_cmd                         ; 4 entries of 8 bits
        pop     dx
        jc      .fail
        push    dx
        mov     cl, dl
        and     cl, 3
        shl     cl, 3
        shr     eax, cl
        pop     dx
        mov     bl, al                          ; BL = candidate node
        mov     al, bl
        call    hda_widget_type
        cmp     al, 0                           ; Audio Output = DAC
        je      .direct
        cmp     al, 2                           ; mixer
        je      .via
        cmp     al, 3                           ; selector
        jne     .next
.via:   ; look one level deeper for a DAC
        mov     al, bl
        call    hda_dac_below
        jc      .next
        mov     [hda_mixer], bl
        ; select this input on the search node
        mov     al, [dac_search_node]
        movzx   edx, dl
        or      edx, 0x70100
        call    hda_cmd
        jmp     .ok
.direct:
        mov     [hda_dac], bl
        mov     al, [dac_search_node]
        movzx   edx, dl
        or      edx, 0x70100                    ; connection select
        call    hda_cmd
        jmp     .ok
.next:  inc     dl
        cmp     dl, [conn_count]
        jb      .entry
.fail:  popad
        stc
        ret
.ok:    popad
        clc
        ret

; hda_dac_below: AL = mixer/selector.  Finds a DAC in its list; sets hda_dac
;   and (for selectors) the connection select.  CF=1 if none.
hda_dac_below:
        pushad
        mov     [dac_search2], al
        mov     cl, 0x0E
        call    hda_param
        jc      .fail
        and     al, 0x7F
        mov     [conn_count2], al
        or      al, al
        jz      .fail
        xor     dl, dl
.entry: push    dx
        movzx   edx, dl
        and     dl, 0xFC
        or      edx, 0xF0200
        mov     al, [dac_search2]
        call    hda_cmd
        pop     dx
        jc      .fail
        push    dx
        mov     cl, dl
        and     cl, 3
        shl     cl, 3
        shr     eax, cl
        pop     dx
        mov     bl, al
        mov     al, bl
        call    hda_widget_type
        cmp     al, 0
        jne     .next
        mov     [hda_dac], bl
        mov     [mixer_input], dl
        mov     al, [dac_search2]
        call    hda_widget_type
        cmp     al, 3
        jne     .ok
        mov     al, [dac_search2]
        movzx   edx, dl
        or      edx, 0x70100
        call    hda_cmd
.ok:    popad
        clc
        ret
.next:  inc     dl
        cmp     dl, [conn_count2]
        jb      .entry
.fail:  popad
        stc
        ret

; hda_unmute: AL = node -> output amp unmuted at ~80% of its range,
;   plus (for mixers) the input amp of the DAC's input
hda_unmute:
        pushad
        mov     bl, al
        mov     cl, 0x12                        ; output amp capabilities
        call    hda_param
        shr     eax, 8
        and     eax, 0x7F                       ; number of steps
        mov     dx, ax
        or      dx, dx
        jnz     .have_steps
        mov     dx, 0x3F
.have_steps:
        mov     ax, dx                          ; full range
        movzx   edx, al
        or      edx, 0x3B000                    ; set amp: output, left+right
        mov     al, bl
        call    hda_cmd
        ; input amps: on the mixer, open only the DAC's input and mute the
        ; others (microphone loopback); elsewhere open everything
        mov     al, bl
        mov     cl, 0x0D                        ; input amp caps
        call    hda_param
        shr     eax, 8
        and     eax, 0x7F
        mov     dx, ax
        or      dx, dx
        jz      .done
        xor     ch, ch
.inputs:
        movzx   edx, ch
        shl     edx, 8
        or      edx, 0x37000                    ; input, left+right, index
        or      dl, 0x1F
        cmp     bl, [hda_mixer]
        jne     .send
        cmp     ch, [mixer_input]
        je      .send
        or      dl, 0x80                        ; mute this input
.send:  mov     al, bl
        call    hda_cmd
        inc     ch
        cmp     ch, 4
        jb      .inputs
.done:  popad
        ret

; hda_setup_path: power up and unmute pin/mixer/DAC, enable the pin output
hda_setup_path:
        pushad
        mov     al, [hda_dac]
        mov     edx, 0x70500
        call    hda_cmd
        mov     al, [hda_pin]
        mov     edx, 0x70500
        call    hda_cmd
        cmp     byte [hda_mixer], 0
        je      .no_mixer
        mov     al, [hda_mixer]
        mov     edx, 0x70500
        call    hda_cmd
        mov     al, [hda_mixer]
        call    hda_unmute
.no_mixer:
        mov     al, [hda_dac]
        call    hda_unmute
        mov     al, [hda_pin]
        call    hda_unmute
        mov     al, [hda_pin]
        mov     edx, 0x707C0                    ; pin control: out + headphone enable
        call    hda_cmd
        mov     al, [hda_pin]
        mov     edx, 0x70C02                    ; EAPD on (external amp)
        call    hda_cmd
        call    hda_enable_beep
        popad
        ret

; -----------------------------------------------------------------------------
; hda_enable_beep: route the codec's PC-beep sources to the output.  The
;   output mixer usually has a second source, the input mixer; in it, open
;   only sources that are internal (no-jack) pins or beep generators, so the
;   8254 speaker line (if the board wires it to the codec) and the codec's
;   own beep generator are heard, but microphones stay muted.
; -----------------------------------------------------------------------------
hda_enable_beep:
        pushad
        cmp     byte [hda_mixer], 0
        je      .done
        mov     al, [hda_mixer]
        mov     cl, 0x0E
        call    hda_param
        and     al, 0x7F
        mov     [beep_count], al
        or      al, al
        jz      .done
        xor     dl, dl                          ; DL = index in the output mixer
.entry: push    dx
        movzx   edx, dl
        and     dl, 0xFC
        or      edx, 0xF0200
        mov     al, [hda_mixer]
        call    hda_cmd
        pop     dx
        jc      .done
        push    dx
        mov     cl, dl
        and     cl, 3
        shl     cl, 3
        shr     eax, cl
        pop     dx
        mov     bl, al                          ; BL = source node
        cmp     bl, [hda_dac]
        je      .next
        mov     al, bl
        call    hda_widget_type
        cmp     al, 2                           ; a mixer: the input mixer
        jne     .next
        mov     [hda_inmix], bl
        movzx   eax, bl
        mov     si, dbg_inmix
        call    dbg
        mov     al, bl
        push    dx
        mov     edx, 0x70500                    ; power on
        call    hda_cmd
        pop     dx
        movzx   edx, dl                         ; open it on the output mixer
        shl     edx, 8
        or      edx, 0x37000
        or      dl, 0x18
        mov     al, [hda_mixer]
        call    hda_cmd
        call    hda_inmix_setup
        jmp     .done
.next:  inc     dl
        cmp     dl, [beep_count]
        jb      .entry
.done:  popad
        ret

; hda_inmix_setup: in the input mixer, open beep-like sources, mute the rest
hda_inmix_setup:
        pushad
        mov     al, [hda_inmix]
        mov     cl, 0x0E
        call    hda_param
        and     al, 0x7F
        mov     [beep_count2], al
        or      al, al
        jz      .done
        mov     al, [hda_inmix]
        mov     edx, 0x3B01F                    ; its output amp: unmute
        call    hda_cmd
        xor     dl, dl
.entry: push    dx
        movzx   edx, dl
        and     dl, 0xFC
        or      edx, 0xF0200
        mov     al, [hda_inmix]
        call    hda_cmd
        pop     dx
        jc      .done
        push    dx
        mov     cl, dl
        and     cl, 3
        shl     cl, 3
        shr     eax, cl
        pop     dx
        mov     bl, al                          ; BL = source node
        mov     al, bl
        call    hda_widget_type
        cmp     al, 7                           ; beep generator
        je      .open
        cmp     al, 4                           ; pin: only if it is no jack
        jne     .mute
        mov     al, bl
        push    dx
        mov     edx, 0xF1C00
        call    hda_cmd
        pop     dx
        jc      .mute
        shr     eax, 30
        cmp     al, 1                           ; "no physical connection"
        jne     .mute
.open:  movzx   eax, bl
        mov     si, dbg_beep_src
        call    dbg
        mov     al, bl
        push    dx
        mov     edx, 0x70500                    ; power on the source
        call    hda_cmd
        pop     dx
        movzx   edx, dl
        shl     edx, 8
        or      edx, 0x37000
        or      dl, 0x1F
        mov     al, [hda_inmix]
        call    hda_cmd
        jmp     .next
.mute:  movzx   edx, dl
        shl     edx, 8
        or      edx, 0x37000
        or      dl, 0x80
        mov     al, [hda_inmix]
        call    hda_cmd
.next:  inc     dl
        cmp     dl, [beep_count2]
        jb      .entry
.done:  popad
        ret

; hda_beep_tone: AX = Hz (0 = off) through the codec's beep generator
hda_beep_tone:
        pushad
        cmp     byte [hda_beep], 0
        je      .done
        or      ax, ax
        jz      .off
        mov     cx, ax
        mov     ax, 12000                       ; tone = 48 kHz / (4 * divider)
        xor     dx, dx
        div     cx
        or      ax, ax
        jnz     .clamp
        inc     ax
.clamp: cmp     ax, 255
        jbe     .set
        mov     ax, 255
.set:   movzx   edx, al
        or      edx, 0x70A00                    ; set beep generation
        mov     al, [hda_beep]
        call    hda_cmd
        jmp     .done
.off:   mov     edx, 0x70A00
        mov     al, [hda_beep]
        call    hda_cmd
.done:  popad
        ret

; =============================================================================
;  Playback
; =============================================================================
; hda_stream_base: EBX = offset of the first output stream descriptor
hda_stream_base:
        push    eax
        mov     ebx, HDA_GCAP
        call    hda_rd16
        shr     ax, 8
        and     ax, 0x0F                        ; number of input streams
        shl     ax, 5
        add     ax, 0x80
        movzx   ebx, ax
        pop     eax
        ret

; hda_stream_reset: reset the output stream descriptor at EBX
hda_stream_reset:
        pushad
        push    ebx
        add     ebx, SD_CTL
        mov     al, 0
        call    hda_wr8                         ; stop
        mov     cx, 2
        call    io_delay_ms
        mov     al, 1                           ; SRST
        call    hda_wr8
        mov     cx, 50
.w1:    call    hda_rd8
        test    al, 1
        jnz     .r1
        push    cx
        mov     cx, 1
        call    io_delay_ms
        pop     cx
        loop    .w1
.r1:    xor     al, al
        call    hda_wr8
        mov     cx, 50
.w2:    call    hda_rd8
        test    al, 1
        jz      .r2
        push    cx
        mov     cx, 1
        call    io_delay_ms
        pop     cx
        loop    .w2
.r2:    pop     ebx
        popad
        ret

; hda_play_start: program the stream for the format in [pcm_fmt] and run it
hda_play_start:
        pushad
        push    es
        call    hda_stream_base                 ; EBX = SD base
        mov     [sd_base], ebx
        call    hda_stream_reset
        ; BDL: two entries covering the two halves
        mov     es, [hda_seg]
        mov     di, BDL_OFF
        movzx   eax, word [pcm_seg]
        shl     eax, 4                          ; physical address of the ring
        mov     [es:di], eax
        mov     dword [es:di+4], 0
        mov     dword [es:di+8], PCM_HALF
        mov     dword [es:di+12], 0
        add     eax, PCM_HALF
        mov     [es:di+16], eax
        mov     dword [es:di+20], 0
        mov     dword [es:di+24], PCM_HALF
        mov     dword [es:di+28], 0
        wbinvd
        mov     ebx, [sd_base]
        push    ebx
        add     ebx, SD_BDPL
        movzx   eax, word [hda_seg]
        shl     eax, 4
        add     eax, BDL_OFF
        call    hda_wr32
        pop     ebx
        push    ebx
        add     ebx, SD_BDPU
        xor     eax, eax
        call    hda_wr32
        pop     ebx
        push    ebx
        add     ebx, SD_CBL
        mov     eax, PCM_HALF * 2
        call    hda_wr32
        pop     ebx
        push    ebx
        add     ebx, SD_LVI
        mov     ax, 1
        call    hda_wr16
        pop     ebx
        push    ebx
        add     ebx, SD_FMT
        mov     ax, [pcm_fmt]
        call    hda_wr16
        pop     ebx
        push    ebx
        add     ebx, SD_CTL2
        mov     al, 0x10                        ; stream number 1
        call    hda_wr8
        pop     ebx
        push    ebx
        add     ebx, SD_STS
        mov     al, 0x1C                        ; clear status bits
        call    hda_wr8
        pop     ebx
        ; codec: converter format, stream 1 channel 0
        mov     al, [hda_dac]
        movzx   edx, word [pcm_fmt]
        or      edx, 0x20000
        call    hda_cmd
        mov     al, [hda_dac]
        mov     edx, 0x70610
        call    hda_cmd
        ; run
        mov     ebx, [sd_base]
        add     ebx, SD_CTL
        mov     al, 0x02
        call    hda_wr8
        pop     es
        popad
        ret

; -----------------------------------------------------------------------------
; pcm_silence: fill the whole ring with nothing.
;   The controller loops that ring for as long as it is running, so whatever
;   is left in it when a sound ends plays on, quietly and forever, until
;   something else is put there.  Stopping the stream ought to settle it, but
;   a stream that does not quite stop then plays the tail of the last sound
;   round and round.  Silence in the buffer settles it either way.
; -----------------------------------------------------------------------------
pcm_silence:
        pushad
        push    es
        cmp     word [pcm_seg], 0
        je      .none
        call    enter_unreal
        mov     es, [pcm_seg]
        xor     di, di
        xor     ax, ax
        mov     cx, PCM_HALF                    ; both halves, in words
        rep     stosw
        wbinvd
.none:  pop     es
        popad
        ret

hda_play_stop:
        pushad
        mov     ebx, [sd_base]
        add     ebx, SD_CTL
        xor     al, al
        call    hda_wr8
        mov     ebx, [sd_base]
        call    hda_stream_reset
        popad
        ret

; hda_position: EAX = link position in the ring buffer (bytes)
hda_position:
        push    ebx
        mov     ebx, [sd_base]
        add     ebx, SD_LPIB
        call    hda_rd32
        pop     ebx
        ret


; =============================================================================
;  PCM stream service for 32-bit programs (INT 21h AH=F0h / F1h)
; =============================================================================
; fF0: start a looping 44.1 kHz 16-bit stereo stream over the PCM ring.
;   The speaker bridge, if it is listening, hands the stream over.
;   DS:DX -> 16-byte block filled with: ring physical address, ring bytes,
;   physical address of the stream's LPIB register, sample rate.  CF on
;   failure (no HD Audio).
fF0:    call    spk_bridge_stop                 ; the program owns the stream now
        mov     es, R_DS
        mov     di, R_DX
        call    snd_stream_start
        push    cs
        pop     es
        jc      .fail
        mov     R_AX, 0
        ret
.fail:  movzx   ax, byte [hda_status]           ; which step of hda_init failed
        mov     R_AX, ax
        jmp     set_cf

fF1:    call    snd_stream_stop
        mov     R_AX, 0
        ret

; fF2: append the ASCIIZ string at DS:DX to EMBER.LOG (for 32-bit programs)
fF2:    push    es
        mov     es, R_DS
        mov     si, R_DX
.next:  mov     al, [es:si]                     ; log_putc needs the kernel DS
        or      al, al
        jz      .end
        call    log_putc
        inc     si
        jmp     .next
.end:   pop     es
        call    log_crlf
        ; The file itself is written when the program ends (gui.asm), not
        ; here: rewriting it for every line is slow, and a desktop that
        ; logs its whole mouse trace on the way out never got out.
        mov     R_AX, 0
        ret

; snd_stream_start: ES:DI -> info block; CF=1 if there is no HDA
snd_stream_start:
        pushad
        push    es
        call    hda_init
        jc      .fail
        call    pcm_acquire                     ; the ring, while it is wanted
        jc      .fail
        call    enter_unreal
        call    hda_setup_path
        push    es
        push    di
        mov     es, [pcm_seg]
        xor     di, di
        xor     ax, ax
        mov     cx, PCM_HALF                    ; both halves, in words
        rep     stosw
        pop     di
        pop     es
        wbinvd
        mov     word [pcm_fmt], 0x4011          ; 44.1 kHz, 16-bit, stereo
        call    hda_play_start
        mov     byte [stream_active], 1
        movzx   eax, word [pcm_seg]
        shl     eax, 4
        mov     [es:di], eax
        mov     dword [es:di+4], PCM_HALF * 2
        mov     eax, [hda_base]
        add     eax, [sd_base]
        add     eax, SD_LPIB
        mov     [es:di+8], eax
        mov     dword [es:di+12], 44100
        pop     es
        popad
        clc
        ret
.fail:  pop     es
        popad
        stc
        ret

snd_stream_stop:
snd_stream_stop_quiet:
        cmp     byte [stream_active], 0
        je      .done
        pushad
        call    pcm_silence                     ; before it stops, not after
        call    enter_unreal
        call    hda_play_stop
        call    pcm_release                     ; and the memory back
        popad
        mov     byte [stream_active], 0
.done:  ret

; =============================================================================
;  Sequential file reader (for streaming WAV data)
; =============================================================================
; fstream_open: found_* -> reader state
fstream_open:
        push    eax
        mov     ax, [found_cluster]
        mov     [fs_cur], ax
        mov     eax, [found_size]
        mov     [fs_left], eax
        mov     byte [fs_sec_in_cl], 0
        mov     word [fs_buf_pos], 512          ; buffer empty
        pop     eax
        ret

; fstream_fill: load the next sector of the stream into fstream_buf.  CF=1 at end
fstream_fill:
        push    eax
        push    bx
        push    es
        mov     ax, ds
        mov     es, ax
        mov     ax, [fs_cur]
        cmp     ax, 2
        jb      .eof
        call    cluster_to_lba
        movzx   ebx, byte [fs_sec_in_cl]
        add     eax, ebx
        mov     bx, fstream_buf
        call    read_sector
        jc      .eof
        inc     byte [fs_sec_in_cl]
        mov     al, [fs_sec_in_cl]
        cmp     al, [fs_spc]
        jb      .same
        mov     byte [fs_sec_in_cl], 0
        mov     ax, [fs_cur]
        call    next_cluster
        jnc     .store
        xor     ax, ax                          ; chain ended
.store: mov     [fs_cur], ax
.same:  mov     word [fs_buf_pos], 0
        pop     es
        pop     bx
        pop     eax
        clc
        ret
.eof:   pop     es
        pop     bx
        pop     eax
        stc
        ret

; fstream_byte: next byte -> AL, CF=1 at end of file
fstream_byte:
        push    bx
        cmp     dword [fs_left], 0
        je      .eof
        mov     bx, [fs_buf_pos]
        cmp     bx, 512
        jb      .have
        call    fstream_fill
        jc      .eof
        xor     bx, bx
.have:  mov     al, [fstream_buf+bx]
        inc     bx
        mov     [fs_buf_pos], bx
        dec     dword [fs_left]
        pop     bx
        clc
        ret
.eof:   pop     bx
        stc
        ret

; fstream_read: copy ECX bytes from the stream to ES:DI, advancing ES across
;   64 KB boundaries.  Stops early at end of file.
fstream_read:
        pushad
        push    es
.loop:  or      ecx, ecx
        jz      .done
        cmp     dword [fs_left], 0
        je      .done
        mov     bx, [fs_buf_pos]
        cmp     bx, 512
        jb      .have
        call    fstream_fill
        jc      .done
        xor     bx, bx
.have:  mov     eax, 512
        sub     ax, bx                          ; bytes left in the buffer
        cmp     eax, [fs_left]
        jbe     .n1
        mov     eax, [fs_left]
.n1:    cmp     eax, ecx
        jbe     .n2
        mov     eax, ecx
.n2:    mov     edx, 0x10000
        movzx   esi, di
        sub     edx, esi                        ; bytes until DI wraps
        cmp     eax, edx
        jbe     .n3
        mov     eax, edx
.n3:    push    cx
        mov     cx, ax
        mov     si, fstream_buf
        add     si, bx
        rep     movsb
        pop     cx
        add     bx, ax
        mov     [fs_buf_pos], bx
        sub     [fs_left], eax
        sub     ecx, eax
        or      di, di
        jnz     .loop
        mov     dx, es                          ; DI wrapped: next segment
        add     dx, 0x1000
        mov     es, dx
        jmp     .loop
.done:  pop     es
        popad
        ret

; fstream_word: next little-endian word -> AX (CF=1 at EOF)
fstream_word:
        call    fstream_byte
        jc      .eof
        mov     ah, al
        call    fstream_byte
        jc      .eof
        xchg    al, ah
        clc
        ret
.eof:   stc
        ret

; fstream_dword: -> EAX
fstream_dword:
        push    bx
        call    fstream_word
        jc      .eof
        mov     bx, ax
        call    fstream_word
        jc      .eof
        shl     eax, 16
        mov     ax, bx
        pop     bx
        clc
        ret
.eof:   pop     bx
        stc
        ret

; fstream_skip: skip ECX bytes
fstream_skip:
        push    ecx
        push    ax
.next:  or      ecx, ecx
        jz      .done
        call    fstream_byte
        jc      .done
        dec     ecx
        jmp     .next
.done:  pop     ax
        pop     ecx
        ret

; =============================================================================
;  WAV player: play_wav plays the file described by found_*.  CF=1 on error
;  (message in wav_error).  Esc stops playback.
; =============================================================================
play_wav:
        pushad
        push    es
        call    hda_init
        jc      .no_hda
        call    enter_unreal                    ; in case something reset GS
        call    hda_setup_path
        mov     byte [wav_finished], 0
        mov     byte [wav_drain], 0
        mov     byte [wav_next_half], 0
        mov     byte [wav_rep_left], 0
        call    fstream_open
        ; ---- RIFF header ----
        mov     word [wav_error], msg_wav_bad
        call    fstream_dword
        cmp     eax, "RIFF"
        jne     .bad
        call    fstream_dword                   ; riff size
        call    fstream_dword
        cmp     eax, "WAVE"
        jne     .bad
        mov     byte [wav_have_fmt], 0
.chunk: call    fstream_dword                   ; chunk id
        jc      .bad
        mov     ebx, eax
        call    fstream_dword                   ; chunk size
        jc      .bad
        mov     ecx, eax
        cmp     ebx, "fmt "
        je      .fmt
        cmp     ebx, "data"
        je      .data
        inc     ecx
        and     ecx, 0xFFFFFFFE                 ; chunks are word aligned
        call    fstream_skip
        jmp     .chunk
.fmt:   call    fstream_word                    ; format tag
        cmp     ax, 1                           ; PCM only
        jne     .bad
        call    fstream_word
        mov     [wav_channels], al
        call    fstream_dword
        mov     [wav_rate], eax
        call    fstream_dword                   ; byte rate
        call    fstream_word                    ; block align
        call    fstream_word
        mov     [wav_bits], al
        sub     ecx, 16
        call    fstream_skip
        mov     byte [wav_have_fmt], 1
        jmp     .chunk
.data:  cmp     byte [wav_have_fmt], 0
        je      .bad
        mov     [wav_data_left], ecx
        cmp     byte [wav_bits], 8
        je      .bits_ok
        cmp     byte [wav_bits], 16
        jne     .bad
.bits_ok:
        cmp     byte [wav_channels], 1
        je      .ch_ok
        cmp     byte [wav_channels], 2
        jne     .bad
.ch_ok: mov     eax, [wav_rate]
        mov     si, dbg_wav_rate
        call    dbg
        movzx   eax, byte [wav_channels]
        shl     eax, 8
        mov     al, [wav_bits]
        mov     si, dbg_wav_fmt
        call    dbg
        call    spk_bridge_stop                 ; borrow the stream
        call    pcm_acquire                     ; and the memory for its ring
        jc      .no_room
        call    wav_rate_to_fmt                 ; -> pcm_fmt, CF=1 if unsupported
        jc      .bad_rate
        ; ---- fill both halves and start ----
        xor     di, di
        call    wav_fill_half
        mov     di, PCM_HALF
        call    wav_fill_half
        call    hda_play_start
        mov     byte [wav_next_half], 0         ; next half to refill
.loop:  ; wait until the hardware has moved past the half we want to refill
        call    hda_position
        cmp     byte [wav_next_half], 0
        jne     .wait_second
        cmp     eax, PCM_HALF                   ; playing the second half?
        jb      .idle
        jmp     .refill
.wait_second:
        cmp     eax, PCM_HALF                   ; back in the first half?
        jae     .idle
.refill:
        mov     si, dbg_refill
        call    dbg                             ; EAX = position (debug only)
        movzx   di, byte [wav_next_half]
        shl     di, 15                          ; 0 or 0x8000
        call    wav_fill_half                   ; data, or silence after the end
        xor     byte [wav_next_half], 1
        cmp     byte [wav_finished], 0
        je      .loop
        dec     byte [wav_drain]                ; let the last data half play out
        jnz     .loop
        jmp     .stop
.idle:  call    kbhit
        jz      .sleep
        call    getkey
        cmp     al, 27
        je      .stop
.sleep: hlt
        jmp     .loop
.stop:  call    pcm_silence                     ; nothing left looping
        call    hda_play_stop
        call    pcm_release
        call    spk_bridge_resume               ; and give it back
        pop     es
        popad
        clc
        ret
.bad_rate:
        mov     word [wav_error], msg_wav_rate
        jmp     .err_free
.bad:   jmp     .err_free
.no_room:
        mov     word [wav_error], msg_no_room
        jmp     .err_free
.no_hda:
        mov     word [wav_error], msg_no_hda
        jmp     .err
.err_free:
        call    pcm_release
.err:   pop     es
        popad
        stc
        ret

; wav_rate_to_fmt: [wav_rate] and the DAC's supported rates -> [pcm_fmt],
;   [wav_repeat] (each source frame is repeated that many times when the
;   codec only accepts a multiple of the file's rate).  CF=1 if unsupported
wav_rate_to_fmt:
        pushad
        mov     al, [hda_dac]
        mov     cl, 0x0A                        ; supported PCM rates
        call    hda_param
        test    ax, 0x0FFF
        jnz     .have_rates
        mov     al, [hda_afg]
        mov     cl, 0x0A
        call    hda_param
        test    ax, 0x0FFF
        jnz     .have_rates
        mov     ax, 0x0060                      ; assume 44.1 and 48 kHz
.have_rates:
        and     eax, 0x0FFF
        mov     [dac_rates], ax
        mov     si, dbg_rates
        call    dbg
        mov     si, rate_table
.find:  mov     eax, [si]
        or      eax, eax
        jz      .unsupported
        cmp     eax, [wav_rate]
        je      .found
        add     si, 8
        jmp     .find
.found: mov     byte [wav_repeat], 1
        mov     cl, [si+6]                      ; bit in the rates word
        cmp     cl, 0xFF
        je      .fallback
        mov     ax, 1
        shl     ax, cl
        test    ax, [dac_rates]
        jz      .fallback
        mov     ax, [si+4]
        mov     [pcm_fmt], ax
        jmp     .ok
.fallback:
        movzx   bx, byte [si+7]                 ; entry to upsample to
        cmp     bl, 0xFF
        je      .unsupported
        shl     bx, 3
        add     bx, rate_table
        mov     eax, [bx]
        xor     edx, edx
        div     dword [wav_rate]                ; repeat factor
        mov     [wav_repeat], al
        mov     ax, [bx+4]
        mov     [pcm_fmt], ax
.ok:    movzx   eax, word [pcm_fmt]
        mov     si, dbg_fmt
        call    dbg
        movzx   eax, byte [wav_repeat]
        mov     si, dbg_repeat
        call    dbg
        popad
        clc
        ret
.unsupported:
        popad
        stc
        ret

; rate, stream format (16-bit stereo + rate bits), rate bit, upsample entry
rate_table:
        dd 48000
        dw 0x0011
        db 6, 0xFF
        dd 44100
        dw 0x4011
        db 5, 0xFF
        dd 32000
        dw 0x0A11
        db 4, 0xFF
        dd 24000
        dw 0x0111
        db 0xFF, 0                              ; -> 48000 x2
        dd 22050
        dw 0x4111
        db 3, 1                                 ; -> 44100 x2
        dd 16000
        dw 0x0211
        db 2, 0                                 ; -> 48000 x3
        dd 11025
        dw 0x4311
        db 1, 1                                 ; -> 44100 x4
        dd 8000
        dw 0x0511
        db 0, 0                                 ; -> 48000 x6
        dd 0

; wav_fill_half: convert PCM_FRAMES frames from the file into PCM_SEG:DI
;   (16-bit stereo); pads with silence and sets wav_finished at the end
wav_fill_half:
        pushad
        push    es
        mov     es, [pcm_seg]
        mov     cx, PCM_FRAMES
.frame: cmp     byte [wav_finished], 0
        jne     .silence
        cmp     byte [wav_rep_left], 0
        jne     .store                          ; repeat the previous frame
        cmp     dword [wav_data_left], 0
        je      .ended
        call    wav_sample                      ; AX = left sample
        jc      .ended
        mov     [wav_cur_l], ax
        mov     [wav_cur_r], ax
        cmp     byte [wav_channels], 1
        je      .got_frame
        call    wav_sample                      ; AX = right sample
        jc      .ended
        mov     [wav_cur_r], ax
.got_frame:
        mov     al, [wav_repeat]
        mov     [wav_rep_left], al
.store: mov     ax, [wav_cur_l]
        mov     [es:di], ax
        mov     ax, [wav_cur_r]
        mov     [es:di+2], ax
        add     di, 4
        dec     byte [wav_rep_left]
        loop    .frame
        jmp     .done
.ended: cmp     byte [wav_finished], 0
        jne     .silence
        mov     byte [wav_finished], 1
        mov     byte [wav_drain], 3             ; this half + two of silence
.silence:
        xor     ax, ax
        mov     [es:di], ax
        mov     [es:di+2], ax
        add     di, 4
        loop    .frame
.done:  wbinvd                                  ; samples must reach RAM for DMA
        pop     es
        popad
        ret

; wav_sample: one sample from the file -> AX (signed 16-bit).  CF=1 at end
wav_sample:
        cmp     byte [wav_bits], 16
        je      .s16
        call    fstream_byte
        jc      .eof
        dec     dword [wav_data_left]
        sub     al, 128
        mov     ah, al
        xor     al, al                          ; (u8 - 128) << 8
        clc
        ret
.s16:   call    fstream_word
        jc      .eof
        sub     dword [wav_data_left], 2
        clc
        ret
.eof:   stc
        ret

; =============================================================================
;  Commands
; =============================================================================
; cmd_play: PLAY <file.wav>  |  PLAY <notes>  |  PLAY (status)
cmd_play:
        cmp     byte [si], 0
        je      cmd_sound
        ; does the argument name a .WAV file?
        push    si
        call    resolve_path
        pop     si
        jc      .notes
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     .notes
        cmp     word [fat_name+8], "WA"
        jne     .notes
        cmp     byte [fat_name+10], 'V'
        jne     .notes
        push    si
        mov     si, msg_playing
        call    puts
        call    log_puts
        pop     si
        call    puts
        call    log_text
        call    crlf
        call    play_wav
        jnc     .played
        mov     si, [wav_error]
        call    puts
        call    log_text
        jmp     .done
.played:
        mov     si, msg_play_done
        call    log_text
.done:  call    log_flush
        ret
.notes: call    play_mml
        ret

; cmd_sound: report what the HDA driver found
cmd_sound:
        mov     byte [snd_debug], 0
        cmp     byte [si], 0
        je      .probe
        mov     byte [snd_debug], 1             ; SOUND DEBUG: trace the probe
        mov     byte [dbg_lines], 0
        mov     byte [hda_ready], 0
.probe: call    hda_init
        jc      .none
        call    enter_unreal
        mov     si, msg_hda_found
        call    puts
        mov     eax, [hda_base]
        call    print_hex32
        mov     si, msg_hda_codec
        call    puts
        movzx   eax, byte [hda_codec]
        call    print_dec
        mov     si, msg_hda_pin
        call    puts
        movzx   eax, byte [hda_pin]
        call    print_dec
        mov     si, msg_hda_dac
        call    puts
        movzx   eax, byte [hda_dac]
        call    print_dec
        cmp     byte [hda_mixer], 0
        je      .no_mixer
        mov     si, msg_hda_mixer
        call    puts
        movzx   eax, byte [hda_mixer]
        call    print_dec
.no_mixer:
        call    crlf
        mov     si, msg_play_usage
        call    puts
        call    log_flush
        ret
.none:  mov     si, msg_no_hda
        call    puts
        call    log_puts
        mov     si, msg_hda_code
        call    puts
        call    log_puts
        movzx   eax, byte [hda_status]
        call    print_dec
        call    log_dec
        call    crlf
        call    log_crlf
        mov     si, msg_play_usage
        call    puts
        call    log_flush
        ret

print_hex32:
        push    eax
        shr     eax, 16
        call    print_hex16
        pop     eax
        push    eax
        call    print_hex16
        pop     eax
        ret

section .data
mml_octave:     db 4
mml_length:     db 4
mml_tempo:      db 120
hda_ready:      db 0
hda_status:     db 0
hda_codec:      db 0
hda_afg:        db 0
hda_pin:        db 0
hda_pin_rank:   db 0
hda_dac:        db 0
hda_mixer:      db 0
hda_inmix:      db 0
hda_beep:       db 0
beep_count:     db 0
beep_count2:    db 0
mixer_input:    db 0
wtype_tmp:      db 0
dac_search_node: db 0
dac_search2:    db 0
conn_count:     db 0
conn_count2:    db 0
                align 4
hda_base:       dd 0
hda_pci:        dd 0
sd_base:        dd 0
corb_wp:        dw 0
rirb_rp:        dw 0
pcm_fmt:        dw 0x0011
stream_active:  db 0
wav_channels:   db 2
wav_bits:       db 16
wav_repeat:     db 1
wav_rep_left:   db 0
wav_cur_l:      dw 0
wav_cur_r:      dw 0
dac_rates:      dw 0
wav_have_fmt:   db 0
wav_finished:   db 0
wav_drain:      db 0
wav_next_half:  db 0
                align 4
wav_rate:       dd 0
wav_data_left:  dd 0
wav_error:      dw msg_wav_bad
fs_cur:         dw 0
fs_sec_in_cl:   db 0
fs_buf_pos:     dw 512
                align 4
fs_left:        dd 0
snd_debug:      db 0
dbg_lines:      db 0
hda_index:      db 0
msg_more:       db "-- press a key for more --", 0
dbg_base:       db "  controller MMIO base  ", 0
dbg_statests:   db "  codec presence mask   ", 0
dbg_root:       db "  root node subnodes    ", 0
dbg_afg:        db "  AFG widgets           ", 0
dbg_pin:        db "  pin widget            ", 0
dbg_cfg:        db "    config default      ", 0
dbg_chosen:     db "  chosen pin            ", 0
dbg_nodac:      db "  no DAC reachable      ", 0
dbg_dac:        db "  DAC                   ", 0
dbg_inmix:      db "  input mixer (beep)    ", 0
dbg_beep_src:   db "    beep source opened  ", 0
dbg_rates:      db "  DAC supported rates   ", 0
dbg_fmt:        db "  stream format         ", 0
dbg_repeat:     db "  upsample factor       ", 0
dbg_wav_rate:   db "  WAV sample rate       ", 0
dbg_wav_fmt:    db "  WAV channels<<8|bits  ", 0
msg_play_done:  db "playback finished", 0
dbg_cmd:        db "      cmd  ", 0
dbg_refill:     db "  refill at position    ", 0
dbg_corbptrs:   db "      TIMEOUT corbrp:wp ", 0
dbg_rirbwp:     db "      rirbwp            ", 0
dbg_rirbsts:    db "      rirbsts           ", 0
dbg_resp:       db "      resp ", 0
msg_playing:    db "Playing ", 0
msg_wav_bad:    db "Not a PCM WAV file (8/16-bit, mono/stereo).", 13, 10, 0
msg_wav_rate:   db "Unsupported sample rate (use 8000-48000 Hz standard rates).", 13, 10, 0
msg_no_room:    db "Not enough memory free for the sound buffer", 13, 10, 0
msg_no_hda:     db "No HD Audio sound hardware found.", 13, 10, 0
msg_hda_code:   db "  (driver status code ", 0
msg_hda_found:  db "HD Audio controller at ", 0
msg_hda_codec:  db "h, codec ", 0
msg_hda_pin:    db ", output pin ", 0
msg_hda_dac:    db ", DAC ", 0
msg_hda_mixer:  db ", via mixer ", 0
msg_play_usage: db "PLAY file.wav        plays a WAV file through the sound chip", 13, 10
                db "PLAY C E G > C       plays notes on the PC speaker (O L T < > . R)", 13, 10, 0
section .bss
fstream_buf:    resb 512
section .text
