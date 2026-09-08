; =============================================================================
;  XMS.MOD - extended memory for DOS programs (XMS 3.0, the useful parts)
; -----------------------------------------------------------------------------
;  A DOS program that wants memory above the first megabyte asks for it the
;  way HIMEM.SYS taught it to: INT 2Fh AX=4300h to see whether anyone is
;  listening, AX=4310h for an address to call, and then far calls with the
;  function in AH.  This provides that.  It is a resident module (LOAD XMS):
;  it hooks INT 2Fh ahead of the kernel and answers those two questions.
;
;  The pool is the upper half of extended memory, so it cannot collide with
;  the 32-bit programs the kernel loads at the one megabyte mark.  Blocks
;  are handed out from a small table of handles, first fit, and coalesced
;  when freed.  Nothing owns extended memory between commands, so the pool is
;  made one free block again every time the shell reaches its prompt: a
;  program that ends without giving its blocks back does not keep them.
;
;  Copying between conventional and extended memory goes through the
;  firmware's own block move (INT 15h AH=87h), which works from real mode
;  and needs no mode switching of our own.  It is not the fastest way, but
;  it is the one that cannot disagree with the rest of the kernel.
;
;  Not provided: the high memory area (no program here needs it), and upper
;  memory blocks.  Both are refused politely, which is a legal answer.
; =============================================================================

[BITS 16]
%define MOD_ORG 0x0010
[ORG MOD_ORG]
%include "ember.inc"

        MODULE_HEADER "XMS     ", xms_init, xms_unload, xms_event, 0

XMS_HANDLES     equ 16                          ; more than any DOS program asks for
XMS_MIN_POOL    equ 0x00200000                  ; do not bother below 2 MB

; =============================================================================
; xms_init: work out the pool, hook INT 2Fh, say what there is
; =============================================================================
xms_init:
        SVC_TABLE_COPY
        ; ---- how much memory is there? ----
        SVC     SVC_EXT_MEM_END                 ; EAX = end of usable memory
        cmp     eax, XMS_MIN_POOL
        jbe     .none
        ; the upper half, aligned down to a megabyte
        mov     ebx, eax
        shr     ebx, 1
        and     ebx, 0xFFF00000
        cmp     ebx, 0x00100000
        jae     .base_ok
        mov     ebx, 0x00100000
.base_ok:
        cmp     ebx, eax
        jae     .none
        mov     [xms_pool_base], ebx
        sub     eax, ebx
        shr     eax, 10                         ; kilobytes
        mov     [xms_pool_size], eax
        call    xms_reset
        ; ---- INT 2Fh: ours first, the old one behind ----
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
        ; ---- say so ----
        mov     si, msg_loaded
        SVC     SVC_PUTS
        mov     eax, [xms_pool_size]
        SVC     SVC_PRINT_DEC
        mov     si, msg_loaded2
        SVC     SVC_PUTS
        mov     si, msg_log
        mov     eax, [xms_pool_base]
        SVC     SVC_LOG_LINE
        clc
        retf
.none:  mov     si, msg_none
        SVC     SVC_PUTS
        stc
        retf

; =============================================================================
; xms_unload: give INT 2Fh back, unless someone has hooked it after us
; =============================================================================
xms_unload:
        push    es
        xor     ax, ax
        mov     es, ax
        cmp     word [es:0x2F*4], int2f_hook
        jne     .busy
        mov     ax, cs
        cmp     [es:0x2F*4+2], ax
        jne     .busy
        cli
        mov     eax, [old_int2f]
        mov     [es:0x2F*4], eax
        sti
        pop     es
        clc
        retf
.busy:  pop     es
        mov     si, msg_hooked
        SVC     SVC_PUTS
        stc
        retf

; =============================================================================
; xms_event: at the prompt, nothing owns extended memory: take it all back
; =============================================================================
xms_event:
        cmp     al, MOD_EV_PROMPT
        jne     .done
        call    xms_reset
.done:  retf

; =============================================================================
; int2f_hook: the two questions HIMEM answers; everything else goes on
; =============================================================================
int2f_hook:
        cmp     ax, 0x4300                      ; is there an XMS driver?
        jne     .not_check
        mov     al, 0x80                        ; there is
        iret
.not_check:
        cmp     ax, 0x4310                      ; where do I call it?
        jne     .not_entry
        push    cs
        pop     es
        mov     bx, xms_entry
        iret
.not_entry:
        jmp     far [cs:old_int2f]

; ---- error codes, as the specification names them ----
XMSERR_HMA      equ 0x90                        ; the HMA does not exist
XMSERR_NOMEM    equ 0xA0                        ; all of it is spoken for
XMSERR_NOHANDLE equ 0xA1                        ; no handles left
XMSERR_HANDLE   equ 0xA2                        ; that is not a handle
XMSERR_SHANDLE  equ 0xA3                        ; the source handle
XMSERR_SOFFSET  equ 0xA4                        ; the source offset
XMSERR_DHANDLE  equ 0xA5                        ; the destination handle
XMSERR_DOFFSET  equ 0xA6                        ; the destination offset
XMSERR_LENGTH   equ 0xA7                        ; the length
XMSERR_LOCKED   equ 0xAB                        ; it is locked
XMSERR_NOTLOCK  equ 0xAA                        ; it is not locked
XMSERR_NOUMB    equ 0xB1                        ; no upper memory blocks

; =============================================================================
; xms_reset: forget every handle and make the pool one free block again
; =============================================================================
xms_reset:
        pusha
        push    es
        push    cs
        pop     es
        mov     di, xms_handle
        mov     cx, XMS_HANDLES * XMS_HANDLE_SZ
        xor     al, al
        cld
        rep     stosb
        cmp     dword [cs:xms_pool_size], 0
        je      .none
        mov     eax, [cs:xms_pool_base]
        mov     [cs:xms_handle + XMS_H_BASE], eax
        mov     eax, [cs:xms_pool_size]
        mov     [cs:xms_handle + XMS_H_SIZE], eax
        mov     byte [cs:xms_handle + XMS_H_STATE], 1   ; 1 = free block
.none:  pop     es
        popa
        ret

; =============================================================================
; the entry point a program far calls, with the function in AH
; =============================================================================
xms_entry:
        cmp     ah, 0x00
        je      xms_f00
        cmp     ah, 0x08
        je      xms_f08
        cmp     ah, 0x09
        je      xms_f09
        cmp     ah, 0x0A
        je      xms_f0a
        cmp     ah, 0x0B
        je      xms_f0b
        cmp     ah, 0x0C
        je      xms_f0c
        cmp     ah, 0x0D
        je      xms_f0d
        cmp     ah, 0x0E
        je      xms_f0e
        cmp     ah, 0x0F
        je      xms_f0f
        cmp     ah, 0x80
        je      xms_f80
        cmp     ah, 0x03                        ; A20 is on and stays on
        jb      .hma
        cmp     ah, 0x07
        jbe     xms_a20
        cmp     ah, 0x10
        jb      .bad
        cmp     ah, 0x12
        jbe     .no_umb
.bad:   xor     ax, ax
        mov     bl, XMSERR_HANDLE
        retf
.hma:   xor     ax, ax                          ; 01h, 02h: there is no HMA
        mov     bl, XMSERR_HMA
        retf
.no_umb:
        xor     ax, ax
        mov     bl, XMSERR_NOUMB
        xor     dx, dx
        retf

; ---- 00h: which version ------------------------------------------------------
xms_f00:
        mov     ax, 0x0300                      ; XMS 3.0
        mov     bx, 0x0001                      ; this driver's own revision
        xor     dx, dx                          ; no high memory area
        retf

; ---- 03h..07h: the A20 line, which this kernel keeps enabled -----------------
xms_a20:
        mov     ax, 1
        cmp     ah, 0x07
        jne     .out
        mov     ax, 1                           ; query: it is enabled
        xor     bl, bl
.out:   xor     bl, bl
        retf

; ---- 08h: how much is free ---------------------------------------------------
;   AX = the largest free block in KB, DX = the total free in KB
xms_f08:
        push    si
        push    cx
        xor     eax, eax                        ; largest
        xor     edx, edx                        ; total
        mov     si, xms_handle
        mov     cx, XMS_HANDLES
.scan:  cmp     byte [cs:si + XMS_H_STATE], 1
        jne     .next
        mov     ebx, [cs:si + XMS_H_SIZE]
        add     edx, ebx
        cmp     ebx, eax
        jbe     .next
        mov     eax, ebx
.next:  add     si, XMS_HANDLE_SZ
        loop    .scan
        ; the interface is 16-bit: anything past 64 MB is reported as 64 MB
        cmp     eax, 0xFFFF
        jbe     .ax_ok
        mov     eax, 0xFFFF
.ax_ok: cmp     edx, 0xFFFF
        jbe     .dx_ok
        mov     edx, 0xFFFF
.dx_ok: pop     cx
        pop     si
        or      ax, ax
        jnz     .some
        mov     bl, XMSERR_NOMEM
        retf
.some:  xor     bl, bl
        retf

; ---- 09h: allocate DX kilobytes ---------------------------------------------
xms_f09:
        push    si
        push    di
        push    cx
        movzx   ebx, dx                         ; wanted, in KB
        or      ebx, ebx
        jnz     .have_size
        mov     ebx, 1                          ; a zero-sized block still needs a handle
.have_size:
        ; first fit among the free blocks
        mov     si, xms_handle
        mov     cx, XMS_HANDLES
.scan:  cmp     byte [cs:si + XMS_H_STATE], 1
        jne     .next
        cmp     [cs:si + XMS_H_SIZE], ebx
        jae     .found
.next:  add     si, XMS_HANDLE_SZ
        loop    .scan
        pop     cx
        pop     di
        pop     si
        xor     ax, ax
        mov     bl, XMSERR_NOMEM
        retf
.found:
        ; exactly the right size?  take it whole
        mov     eax, [cs:si + XMS_H_SIZE]
        sub     eax, ebx
        jz      .take_whole
        ; otherwise split: a free handle holds the remainder
        call    xms_free_slot                   ; DI = a spare slot, CF on failure
        jc      .no_handle
        mov     [cs:di + XMS_H_SIZE], eax       ; the remainder
        mov     eax, [cs:si + XMS_H_BASE]
        add     eax, ebx
        shl     eax, 10                         ; KB -> bytes for the base
        mov     edx, ebx
        shl     edx, 10
        mov     eax, [cs:si + XMS_H_BASE]
        add     eax, edx
        mov     [cs:di + XMS_H_BASE], eax
        mov     byte [cs:di + XMS_H_STATE], 1
        mov     [cs:si + XMS_H_SIZE], ebx
.take_whole:
        mov     byte [cs:si + XMS_H_STATE], 2   ; 2 = in use
        mov     byte [cs:si + XMS_H_LOCKS], 0
        mov     ax, si
        sub     ax, xms_handle
        mov     bl, XMS_HANDLE_SZ
        div     bl                              ; AL = index
        movzx   dx, al
        inc     dx                              ; handles are numbered from one
        pop     cx
        pop     di
        pop     si
        mov     ax, 1
        xor     bl, bl
        retf
.no_handle:
        pop     cx
        pop     di
        pop     si
        xor     ax, ax
        mov     bl, XMSERR_NOHANDLE
        retf

; ---- 0Ah: free the block DX names -------------------------------------------
xms_f0a:
        call    xms_find                        ; SI = the entry, CF if not one
        jc      .bad
        cmp     byte [cs:si + XMS_H_LOCKS], 0
        jne     .locked
        mov     byte [cs:si + XMS_H_STATE], 1   ; free again
        call    xms_coalesce
        mov     ax, 1
        xor     bl, bl
        retf
.bad:   xor     ax, ax
        mov     bl, XMSERR_HANDLE
        retf
.locked:
        xor     ax, ax
        mov     bl, XMSERR_LOCKED
        retf

; ---- 0Bh: move a block ------------------------------------------------------
;   DS:SI -> length (4), source handle (2), source offset (4),
;            destination handle (2), destination offset (4)
xms_f0b:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    cx
        push    dx
        push    ds
        push    es
        mov     eax, [si]                       ; the length, in bytes
        mov     [cs:xms_mv_len], eax
        test    eax, 1
        jnz     .bad_len
        mov     dx, [si+4]                      ; source handle
        mov     eax, [si+6]                     ; source offset
        call    xms_linear                      ; -> EAX, CF on a bad handle
        jc      .bad_src
        mov     [cs:xms_mv_src], eax
        mov     dx, [si+10]
        mov     eax, [si+12]
        call    xms_linear
        jc      .bad_dst
        mov     [cs:xms_mv_dst], eax
        mov     eax, [cs:xms_mv_len]
        or      eax, eax
        jz      .done                           ; nothing to move is success
.chunk:
        ; up to 64 KB at a time, which the firmware's move is happy with
        mov     ecx, [cs:xms_mv_len]
        cmp     ecx, 0x10000
        jbe     .have_chunk
        mov     ecx, 0x10000
.have_chunk:
        push    ecx
        shr     ecx, 1                          ; words
        call    xms_block_move                  ; CF on failure
        pop     eax
        jc      .failed
        add     [cs:xms_mv_src], eax
        add     [cs:xms_mv_dst], eax
        sub     [cs:xms_mv_len], eax
        jnz     .chunk
.done:
        pop     es
        pop     ds
        pop     dx
        pop     cx
        pop     di
        pop     si
        pop     bp
        mov     ax, 1
        xor     bl, bl
        retf
.bad_len:
        mov     bl, XMSERR_LENGTH
        jmp     .fail
.bad_src:
        mov     bl, XMSERR_SHANDLE
        jmp     .fail
.bad_dst:
        mov     bl, XMSERR_DHANDLE
        jmp     .fail
.failed:
        mov     bl, XMSERR_LENGTH
.fail:
        pop     es
        pop     ds
        pop     dx
        pop     cx
        pop     di
        pop     si
        pop     bp
        xor     ax, ax
        retf

; ---- 0Ch / 0Dh: lock and unlock ---------------------------------------------
xms_f0c:
        call    xms_find
        jc      .bad
        inc     byte [cs:si + XMS_H_LOCKS]
        mov     eax, [cs:si + XMS_H_BASE]
        mov     bx, ax
        shr     eax, 16
        mov     dx, ax
        mov     ax, 1
        retf
.bad:   xor     ax, ax
        mov     bl, XMSERR_HANDLE
        retf

xms_f0d:
        call    xms_find
        jc      .bad
        cmp     byte [cs:si + XMS_H_LOCKS], 0
        je      .not_locked
        dec     byte [cs:si + XMS_H_LOCKS]
        mov     ax, 1
        xor     bl, bl
        retf
.bad:   xor     ax, ax
        mov     bl, XMSERR_HANDLE
        retf
.not_locked:
        xor     ax, ax
        mov     bl, XMSERR_NOTLOCK
        retf

; ---- 0Eh: what is this handle ------------------------------------------------
xms_f0e:
        push    cx
        push    di
        call    xms_find
        jc      .bad
        mov     bh, [cs:si + XMS_H_LOCKS]
        ; how many slots are still spare
        xor     bl, bl
        mov     di, xms_handle
        mov     cx, XMS_HANDLES
.count: cmp     byte [cs:di + XMS_H_STATE], 0
        jne     .next
        inc     bl
.next:  add     di, XMS_HANDLE_SZ
        loop    .count
        mov     eax, [cs:si + XMS_H_SIZE]
        cmp     eax, 0xFFFF
        jbe     .size_ok
        mov     eax, 0xFFFF
.size_ok:
        mov     dx, ax
        pop     di
        pop     cx
        mov     ax, 1
        retf
.bad:   pop     di
        pop     cx
        xor     ax, ax
        mov     bl, XMSERR_HANDLE
        retf

; ---- 0Fh: resize ------------------------------------------------------------
;   Growing would mean moving somebody else, so only shrinking is offered.
xms_f0f:
        call    xms_find
        jc      .bad
        movzx   eax, bx                         ; the new size, in KB
        cmp     eax, [cs:si + XMS_H_SIZE]
        ja      .no_room
        mov     ax, 1
        xor     bl, bl
        retf
.bad:   xor     ax, ax
        mov     bl, XMSERR_HANDLE
        retf
.no_room:
        xor     ax, ax
        mov     bl, XMSERR_NOMEM
        retf

; ---- 80h: ours alone: where the pool begins ---------------------------------
;   The extended memory below the pool, from the megabyte up, is nobody's
;   while a DOS program runs: the kernel loads its 32-bit programs there,
;   and none is resident then.  The DPMI host asks so it can use it.
;   DX:BX = the pool's base, ECX = its size in KB, AX = 1.
xms_f80:
        mov     eax, [cs:xms_pool_base]
        mov     bx, ax
        shr     eax, 16
        mov     dx, ax
        mov     ecx, [cs:xms_pool_size]
        mov     ax, 1
        retf

; =============================================================================
; helpers
; =============================================================================
; xms_find: DX = handle -> SI = its entry.  CF set when it is not one.
xms_find:
        push    ax
        or      dx, dx
        jz      .bad
        cmp     dx, XMS_HANDLES
        ja      .bad
        mov     ax, dx
        dec     ax
        mov     si, XMS_HANDLE_SZ
        mul     si
        mov     si, xms_handle
        add     si, ax
        cmp     byte [cs:si + XMS_H_STATE], 2   ; must be one in use
        jne     .bad
        pop     ax
        clc
        ret
.bad:   pop     ax
        stc
        ret

; xms_free_slot: DI = an unused table slot.  CF set when there is none.
xms_free_slot:
        push    cx
        mov     di, xms_handle
        mov     cx, XMS_HANDLES
.scan:  cmp     byte [cs:di + XMS_H_STATE], 0
        je      .found
        add     di, XMS_HANDLE_SZ
        loop    .scan
        pop     cx
        stc
        ret
.found: pop     cx
        clc
        ret

; xms_coalesce: join free blocks that touch, so a freed block can be reused
xms_coalesce:
        pusha
        mov     si, xms_handle
        mov     cx, XMS_HANDLES
.outer: cmp     byte [cs:si + XMS_H_STATE], 1
        jne     .next_outer
        push    cx
        mov     di, xms_handle
        mov     cx, XMS_HANDLES
.inner: cmp     byte [cs:di + XMS_H_STATE], 1
        jne     .next_inner
        cmp     di, si
        je      .next_inner
        ; does DI start where SI ends?
        mov     eax, [cs:si + XMS_H_SIZE]
        shl     eax, 10
        add     eax, [cs:si + XMS_H_BASE]
        cmp     eax, [cs:di + XMS_H_BASE]
        jne     .next_inner
        mov     eax, [cs:di + XMS_H_SIZE]
        add     [cs:si + XMS_H_SIZE], eax
        mov     byte [cs:di + XMS_H_STATE], 0   ; the slot goes back
.next_inner:
        add     di, XMS_HANDLE_SZ
        loop    .inner
        pop     cx
.next_outer:
        add     si, XMS_HANDLE_SZ
        loop    .outer
        popa
        ret

; xms_linear: DX = handle (0 means conventional), EAX = offset
;             -> EAX = a linear address.  CF set when the handle is wrong.
xms_linear:
        push    si
        or      dx, dx
        jnz     .extended
        ; conventional: the offset is a segment in the high half, an offset low
        push    ebx
        mov     ebx, eax
        shr     ebx, 16                         ; segment
        and     eax, 0xFFFF                     ; offset
        shl     ebx, 4
        add     eax, ebx
        pop     ebx
        pop     si
        clc
        ret
.extended:
        call    xms_find
        jc      .bad
        add     eax, [cs:si + XMS_H_BASE]
        pop     si
        clc
        ret
.bad:   pop     si
        stc
        ret

; xms_block_move: ECX words from [xms_mv_src] to [xms_mv_dst], through the
; firmware's own move, which is the one thing that reaches above a megabyte
; from real mode without this kernel changing modes underneath itself.
xms_block_move:
        pusha
        push    es
        push    cs
        pop     es
        ; ---- the little descriptor table the firmware wants ----
        mov     di, xms_gdt
        mov     cx, 8 * 8 / 2
        xor     ax, ax
        cld
        rep     stosw
        ; source
        mov     word [cs:xms_gdt + 2*8 + 0], 0xFFFF
        mov     eax, [cs:xms_mv_src]
        mov     [cs:xms_gdt + 2*8 + 2], ax
        shr     eax, 16
        mov     [cs:xms_gdt + 2*8 + 4], al
        mov     byte [cs:xms_gdt + 2*8 + 5], 0x93
        mov     [cs:xms_gdt + 2*8 + 7], ah
        ; destination
        mov     word [cs:xms_gdt + 3*8 + 0], 0xFFFF
        mov     eax, [cs:xms_mv_dst]
        mov     [cs:xms_gdt + 3*8 + 2], ax
        shr     eax, 16
        mov     [cs:xms_gdt + 3*8 + 4], al
        mov     byte [cs:xms_gdt + 3*8 + 5], 0x93
        mov     [cs:xms_gdt + 3*8 + 7], ah
        pop     es
        popa
        pusha
        push    es
        push    si
        push    cs
        pop     es
        mov     si, xms_gdt
        mov     ah, 0x87
        int     0x15
        pop     si
        pop     es
        jc      .failed
        popa
        clc
        ret
.failed:
        popa
        stc
        ret

; =============================================================================
section .data
; the handle table: state, locks, base (bytes), size (KB)
XMS_H_STATE     equ 0                           ; 0 spare, 1 free block, 2 in use
XMS_H_LOCKS     equ 1
XMS_H_BASE      equ 2
XMS_H_SIZE      equ 6
XMS_HANDLE_SZ   equ 10

xms_handle:     times XMS_HANDLES * XMS_HANDLE_SZ db 0
xms_pool_base:  dd 0
xms_pool_size:  dd 0
xms_mv_len:     dd 0
xms_mv_src:     dd 0
xms_mv_dst:     dd 0
xms_gdt:        times 8 * 8 db 0
old_int2f:      dd 0                            ; the vector we chained to
svc_table:      times SVC_MAX * 4 db 0          ; the kernel's services, copied
msg_loaded:     db "XMS: ", 0
msg_loaded2:    db " KB of extended memory", 13, 10, 0
msg_none:       db "XMS: no extended memory to manage", 13, 10, 0
msg_hooked:     db "XMS: INT 2Fh has been hooked by something else", 13, 10, 0
msg_log:        db "XMS pool base           ", 0
section .text
