; =============================================================================
;  mem.asm - conventional memory: service buffers at the top, and a DOS-style
;            arena of MCB-headed blocks for programs in between
; -----------------------------------------------------------------------------
;  Layout (segments), computed at boot from the BIOS memory size (INT 12h):
;     0x0800  kernel (64 KB)
;     0x1800  arena: first MCB ... up to arena_end
;     pcm_seg   64 KB  HD Audio PCM ring buffer   (borrowed on demand)
;     gui_seg   48 KB  GUI scratch                 (borrowed on demand)
;     hda_seg    4 KB  CORB / RIRB / BDL
;     stk_seg    8 KB  the INT 21h stack, its register frame at the top,
;                     the saved interrupt vectors in its bottom 1 KB
;     log_seg    8 KB  boot log
;     top       EBDA / end of conventional memory
;
;  MCB (memory control block), 16 bytes before every block, as in DOS:
;     +0  'M' (more follow) or 'Z' (last)     +1  owner PSP (0 = free, 8 = system)
;     +3  size in paragraphs                  +8  owner name (8 bytes)
; =============================================================================

ARENA_START     equ 0x1800
MCB_SIG         equ 0
MCB_OWNER       equ 1
MCB_SIZE        equ 3
MCB_NAME        equ 8
OWNER_SYSTEM    equ 8

; -----------------------------------------------------------------------------
; mem_init: place the service buffers and create the arena
; -----------------------------------------------------------------------------
mem_init:
        pusha
        push    es
        int     0x12                            ; AX = KB of conventional memory
        cmp     ax, 512
        jae     .size_ok
        mov     ax, 512
.size_ok:
        shl     ax, 6                           ; -> segment of the top
        mov     [mem_top_seg], ax
        sub     ax, 0x0200                      ; 8 KB log
        mov     [log_seg], ax
        sub     ax, 0x0100                      ; 4 KB HDA rings
        mov     [hda_seg], ax
        sub     ax, 0x0200                      ; 8 KB INT 21h stack and frame
        mov     [stk_seg], ax
        mov     [arena_end], ax
        mov     word [gui_seg], 0               ; the big two are borrowed from
        mov     word [pcm_seg], 0               ;  the arena only when wanted
        ; one free block spanning the arena
        mov     es, [arena_first]
        mov     byte [es:MCB_SIG], 'Z'
        mov     word [es:MCB_OWNER], 0
        sub     ax, ARENA_START
        dec     ax
        mov     [es:MCB_SIZE], ax
        xor     di, di
        mov     di, MCB_NAME
        mov     cx, 8
        xor     al, al
        rep     stosb
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; mem_alloc: BX = paragraphs -> AX = segment of the block.  CF=1 if none;
;   then BX = largest available.  Owner = current PSP (or system).
; -----------------------------------------------------------------------------
mem_alloc:
        push    cx
        push    dx
        push    es
        push    fs
        mov     dx, [arena_first]
        xor     cx, cx                          ; CX = largest free seen
.walk:  mov     es, dx
        cmp     word [es:MCB_OWNER], 0
        jne     .next
        mov     ax, [es:MCB_SIZE]
        cmp     ax, cx
        jbe     .not_larger
        mov     cx, ax
.not_larger:
        cmp     ax, bx
        jb      .next
        ; fits: split if there is room for another MCB + at least 1 paragraph
        sub     ax, bx
        cmp     ax, 2
        jb      .take_all
        push    dx
        add     dx, bx
        inc     dx
        mov     fs, dx                          ; FS = new free MCB after ours
        pop     dx
        mov     cl, [es:MCB_SIG]
        mov     [fs:MCB_SIG], cl
        mov     word [fs:MCB_OWNER], 0
        dec     ax
        mov     [fs:MCB_SIZE], ax
        mov     byte [es:MCB_SIG], 'M'
        mov     [es:MCB_SIZE], bx
.take_all:
        mov     ax, [cur_psp]
        or      ax, ax
        jnz     .owner
        mov     ax, OWNER_SYSTEM
.owner: mov     [es:MCB_OWNER], ax
        mov     ax, dx
        inc     ax                              ; data starts after the MCB
        pop     fs
        pop     es
        pop     dx
        pop     cx
        clc
        ret
.next:  cmp     byte [es:MCB_SIG], 'Z'
        je      .fail
        add     dx, [es:MCB_SIZE]
        inc     dx
        jmp     .walk
.fail:  mov     bx, cx
        pop     fs
        pop     es
        pop     dx
        pop     cx
        stc
        ret

; mem_set_owner: ES = block, AX = new owner PSP
mem_set_owner:
        push    es
        push    bx
        mov     bx, es
        dec     bx
        mov     es, bx
        mov     [es:MCB_OWNER], ax
        pop     bx
        pop     es
        ret

; -----------------------------------------------------------------------------
; mem_alloc_system: like mem_alloc, but the block belongs to the system and
;   so survives the program that happened to be running when it was taken.
; -----------------------------------------------------------------------------
mem_alloc_system:
        push    dx
        mov     dx, [cur_psp]
        mov     word [cur_psp], 0
        call    mem_alloc
        mov     [cur_psp], dx
        pop     dx
        ret

; -----------------------------------------------------------------------------
; pcm_acquire / pcm_release: the 64 KB ring the HD Audio controller reads.
; gui_scratch_get / gui_scratch_put: the 48 KB the graphical shell keeps its lists in.
;   CF=1 from either acquire if the memory is not free.
; -----------------------------------------------------------------------------
pcm_acquire:
        cmp     word [pcm_seg], 0
        jne     .have
        push    bx
        mov     bx, 0x1000                      ; 64 KB
        call    mem_alloc_system
        pop     bx
        jc      .fail
        mov     [pcm_seg], ax
.have:  clc
        ret
.fail:  stc
        ret

pcm_release:
        cmp     word [pcm_seg], 0
        je      .done
        push    es
        mov     es, [pcm_seg]
        call    mem_free
        pop     es
        mov     word [pcm_seg], 0
.done:  ret

gui_scratch_get:
        cmp     word [gui_seg], 0
        jne     .have
        push    bx
        mov     bx, 0x0C00                      ; 48 KB
        call    mem_alloc_system
        pop     bx
        jc      .fail
        mov     [gui_seg], ax
.have:  clc
        ret
.fail:  stc
        ret

gui_scratch_put:
        cmp     word [gui_seg], 0
        je      .done
        push    es
        mov     es, [gui_seg]
        call    mem_free
        pop     es
        mov     word [gui_seg], 0
.done:  ret

; -----------------------------------------------------------------------------
; mem_free: ES = block segment.  CF=1 if not a valid block
; -----------------------------------------------------------------------------
mem_free:
        push    ax
        push    es
        mov     ax, es
        dec     ax
        mov     es, ax
        cmp     byte [es:MCB_SIG], 'M'
        je      .ok
        cmp     byte [es:MCB_SIG], 'Z'
        jne     .bad
.ok:    mov     word [es:MCB_OWNER], 0
        call    mem_coalesce
        pop     es
        pop     ax
        clc
        ret
.bad:   pop     es
        pop     ax
        stc
        ret

; -----------------------------------------------------------------------------
; mem_coalesce: merge adjacent free blocks
; -----------------------------------------------------------------------------
mem_coalesce:
        pusha
        push    es
        push    fs
        mov     dx, [arena_first]
.walk:  mov     es, dx
        cmp     byte [es:MCB_SIG], 'Z'
        je      .done
        cmp     word [es:MCB_OWNER], 0
        jne     .advance
        ; next block
        mov     ax, dx
        add     ax, [es:MCB_SIZE]
        inc     ax
        mov     fs, ax
        cmp     word [fs:MCB_OWNER], 0
        jne     .advance
        ; merge next into this
        mov     ax, [fs:MCB_SIZE]
        inc     ax
        add     [es:MCB_SIZE], ax
        mov     al, [fs:MCB_SIG]
        mov     [es:MCB_SIG], al
        jmp     .walk                           ; try merging again
.advance:
        add     dx, [es:MCB_SIZE]
        inc     dx
        jmp     .walk
.done:  pop     fs
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; mem_resize: ES = block, BX = new size in paragraphs.  CF=1 if impossible
;   (then BX = largest size this block could have)
; -----------------------------------------------------------------------------
mem_resize:
        push    ax
        push    cx
        push    dx
        push    es
        push    fs
        mov     ax, es
        dec     ax
        mov     es, ax                          ; ES = this MCB
        mov     cx, [es:MCB_SIZE]
        cmp     bx, cx
        je      .ok
        jb      .shrink
        ; grow: need a free next block
        cmp     byte [es:MCB_SIG], 'Z'
        je      .too_big
        mov     dx, ax
        add     dx, cx
        inc     dx
        mov     fs, dx                          ; FS = next MCB
        cmp     word [fs:MCB_OWNER], 0
        jne     .too_big
        mov     dx, cx
        add     dx, [fs:MCB_SIZE]
        inc     dx                              ; DX = size if merged
        cmp     bx, dx
        ja      .too_big_merged
        ; merge, then shrink to BX
        mov     [es:MCB_SIZE], dx
        mov     dl, [fs:MCB_SIG]
        mov     [es:MCB_SIG], dl
        mov     cx, [es:MCB_SIZE]
        cmp     bx, cx
        je      .ok
.shrink:
        ; split: new free MCB after BX paragraphs
        mov     dx, cx
        sub     dx, bx                          ; leftover including the new MCB
        cmp     dx, 1
        jbe     .ok                             ; not worth a new block
        mov     [es:MCB_SIZE], bx
        mov     cx, ax
        add     cx, bx
        inc     cx
        mov     fs, cx                          ; FS = new MCB
        mov     cl, [es:MCB_SIG]
        mov     [fs:MCB_SIG], cl
        mov     byte [es:MCB_SIG], 'M'
        mov     word [fs:MCB_OWNER], 0
        dec     dx
        mov     [fs:MCB_SIZE], dx
        call    mem_coalesce
.ok:    pop     fs
        pop     es
        pop     dx
        pop     cx
        pop     ax
        clc
        ret
.too_big_merged:
        mov     bx, dx
        jmp     .fail
.too_big:
        mov     bx, cx
.fail:  pop     fs
        pop     es
        pop     dx
        pop     cx
        pop     ax
        stc
        ret

; -----------------------------------------------------------------------------
; mem_free_owner: free every block owned by PSP AX
; -----------------------------------------------------------------------------
mem_free_owner:
        pusha
        push    es
        mov     dx, [arena_first]
.walk:  mov     es, dx
        cmp     [es:MCB_OWNER], ax
        jne     .next
        mov     word [es:MCB_OWNER], 0
.next:  cmp     byte [es:MCB_SIG], 'Z'
        je      .done
        add     dx, [es:MCB_SIZE]
        inc     dx
        jmp     .walk
.done:  call    mem_coalesce
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; mem_free_programs: free every block not owned by the system (used when the
;   last program has ended, so nothing a program allocated can leak)
; -----------------------------------------------------------------------------
mem_free_programs:
        pusha
        push    es
        mov     dx, [arena_first]
.walk:  mov     es, dx
        cmp     word [es:MCB_OWNER], OWNER_SYSTEM
        je      .next
        mov     word [es:MCB_OWNER], 0
.next:  cmp     byte [es:MCB_SIG], 'Z'
        je      .done
        add     dx, [es:MCB_SIZE]
        inc     dx
        jmp     .walk
.done:  call    mem_coalesce
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; mem_largest: BX = largest free block in paragraphs
; -----------------------------------------------------------------------------
mem_largest:
        push    ax
        push    dx
        push    es
        xor     bx, bx
        mov     dx, [arena_first]
.walk:  mov     es, dx
        cmp     word [es:MCB_OWNER], 0
        jne     .next
        mov     ax, [es:MCB_SIZE]
        cmp     ax, bx
        jbe     .next
        mov     bx, ax
.next:  cmp     byte [es:MCB_SIG], 'Z'
        je      .done
        add     dx, [es:MCB_SIZE]
        inc     dx
        jmp     .walk
.done:  pop     es
        pop     dx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; mem_free_total: EAX = free bytes in the arena
; -----------------------------------------------------------------------------
mem_free_total:
        push    bx
        push    dx
        push    es
        xor     eax, eax
        mov     dx, [arena_first]
.walk:  mov     es, dx
        cmp     word [es:MCB_OWNER], 0
        jne     .next
        movzx   ebx, word [es:MCB_SIZE]
        add     eax, ebx
.next:  cmp     byte [es:MCB_SIG], 'Z'
        je      .done
        add     dx, [es:MCB_SIZE]
        inc     dx
        jmp     .walk
.done:  shl     eax, 4
        pop     es
        pop     dx
        pop     bx
        ret

section .data
mem_top_seg:    dw 0xA000
arena_first:    dw ARENA_START
arena_end:      dw 0x8000
pcm_seg:        dw 0x8000
gui_seg:        dw 0x9000
hda_seg:        dw 0x9C00
stk_seg:        dw 0x9A00                       ; INT 21h runs on this
log_seg:        dw 0x9D00
section .text
