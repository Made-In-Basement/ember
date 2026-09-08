; =============================================================================
;  module.asm - resident modules: drivers that live in a segment of their own
; -----------------------------------------------------------------------------
;  The kernel's single 64 KB segment is full.  Anything that stays resident
;  from now on - a memory manager, a DPMI host, a device driver - is a module:
;  a flat binary with a 32-byte header, loaded from a file into a block of
;  conventional memory that belongs to the system, so it survives the programs
;  that come and go.  LOAD puts one in place, UNLOAD takes it out again.
;
;  The header (see modules/ember.inc, which modules include):
;     +0   "EMOD"                     +4   header version (1)
;     +6   flags (unused)             +8   name, 8 bytes, upper case, padded
;     +16  init entry                 +18  unload entry (0 = none)
;     +20  event entry (0 = none)     +22  paragraphs wanted (0 = the file)
;     +24  paragraphs kept after init (0 = all)
;     +26  far pointer to the kernel's service table, written by the loader
;     +30  reserved                   +32  code
;
;  The kernel far-calls the entries with DS = the module's segment:
;     init    ES:SI = the rest of the command line.  CF=1 refuses the load.
;     unload  CF=1 refuses (a vector of its is hooked by someone else, say).
;     event   AL = MOD_EV_PROMPT when the shell has the machine to itself,
;             MOD_EV_START just before a program runs, MOD_EV_END after it.
;
;  What a module gets from the kernel is the service table: a count and then
;  far pointers, one per service, numbered in ember.inc.  A module copies the
;  table into its own segment at init and far-calls through it.
; =============================================================================

MOD_MAX         equ 8
MH_MAGIC        equ 0
MH_VERSION      equ 4
MH_FLAGS        equ 6
MH_NAME         equ 8
MH_INIT         equ 16
MH_UNLOAD       equ 18
MH_EVENT        equ 20
MH_PARAS        equ 22
MH_RESIDENT     equ 24
MH_SERVICES     equ 26
MH_SIZE         equ 32

MOD_EV_PROMPT   equ 0
MOD_EV_START    equ 1
MOD_EV_END      equ 2

; -----------------------------------------------------------------------------
; cmd_load: LOAD [name [arguments]].  Alone, it lists what is loaded.
; -----------------------------------------------------------------------------
cmd_load:
        cmp     byte [si], 0
        je      mod_list
        call    mod_load
        ret

; -----------------------------------------------------------------------------
; cmd_unload: UNLOAD name
; -----------------------------------------------------------------------------
cmd_unload:
        cmp     byte [si], 0
        je      .usage
        call    mod_unload
        ret
.usage: mov     si, msg_mod_unload_usage
        call    puts
        ret

; -----------------------------------------------------------------------------
; mod_list: one line per loaded module
; -----------------------------------------------------------------------------
mod_list:
        mov     bx, mod_table
        cmp     word [bx], 0
        jne     .some
        mov     si, msg_mod_none
        call    puts
        ret
.some:  mov     si, msg_mod_head
        call    puts
.next:  mov     ax, [bx]
        or      ax, ax
        jz      .done
        push    bx
        mov     es, ax
        mov     al, ' '
        call    putc
        call    putc
        mov     si, MH_NAME
        mov     cx, 8
        call    mod_puts_es                     ; the name
        mov     al, ' '
        call    putc
        call    putc
        mov     ax, es
        call    print_hex16                     ; its segment
        mov     ax, es
        dec     ax
        mov     es, ax                          ; its MCB: the size in paragraphs
        movzx   eax, word [es:MCB_SIZE]
        add     eax, 63
        shr     eax, 6                          ; -> KB, rounded up
        mov     cl, 6
        call    print_dec_pad
        mov     si, msg_mod_kb
        call    puts
        pop     bx
        add     bx, 2
        jmp     .next
.done:  mov     ax, cs
        mov     es, ax
        ret

; mod_puts_es: print CX bytes at ES:SI
mod_puts_es:
        push    ax
.loop:  mov     al, [es:si]
        inc     si
        call    putc
        loop    .loop
        pop     ax
        ret

; -----------------------------------------------------------------------------
; mod_load: DS:SI = "name [arguments]".  Loads NAME.MOD (a path is accepted,
;   and a bare name is also looked for in the root), calls its init, keeps it.
; -----------------------------------------------------------------------------
mod_load:
        cmp     byte [fs_ok], 0
        je      .not_found
        ; ---- the file name, and where the arguments start ----
        mov     di, path_buf
        xor     bl, bl                          ; BL = the last part has a dot
        xor     bh, bh                          ; BH = there was a separator
        xor     cx, cx
.copy:  lodsb
        cmp     al, ' '
        je      .name_end
        cmp     al, 9
        je      .name_end
        or      al, al
        jz      .name_end
        cmp     al, '\'
        je      .sep
        cmp     al, '/'
        je      .sep
        cmp     al, '.'
        jne     .store
        mov     bl, 1
        jmp     .store
.sep:   xor     bl, bl
        mov     bh, 1
.store: call    upcase
        stosb
        inc     cx
        cmp     cx, 70
        jb      .copy
.name_end:
        dec     si
        call    skip_spaces
        mov     [mod_args], si
        mov     byte [di], 0
        test    bl, bl
        jnz     .try_as_is
        mov     dword [di], ".MOD"
        mov     byte [di+4], 0
.try_as_is:
        mov     si, path_buf
        call    resolve_path
        jnc     .found
        test    bh, bh
        jnz     .not_found
        ; a bare name: try the root
        mov     si, path_buf
        mov     di, tmp_path
        mov     al, '\'
        stosb
.root:  lodsb
        stosb
        or      al, al
        jnz     .root
        mov     si, tmp_path
        call    resolve_path
        jc      .not_found
.found: test    byte [found_attr], ATTR_DIRECTORY
        jnz     .not_found
        ; ---- the header ----
        cmp     dword [found_size], MH_SIZE
        jb      .bad_format
        cmp     dword [found_size], 0x10000
        ja      .bad_format
        call    fstream_open
        mov     di, mod_hdr
        mov     ecx, MH_SIZE
        call    fstream_read
        cmp     dword [mod_hdr + MH_MAGIC], "EMOD"
        jne     .bad_format
        cmp     word [mod_hdr + MH_VERSION], 1
        jne     .bad_format
        ; ---- already loaded? ----
        mov     si, mod_hdr + MH_NAME
        call    mod_find_name                   ; BX -> its table entry
        jnc     .already
        ; ---- a free table entry ----
        mov     bx, mod_table
.slot:  cmp     word [bx], 0
        je      .have_slot
        add     bx, 2
        cmp     bx, mod_table + MOD_MAX * 2
        jb      .slot
        mov     si, msg_mod_full
        call    puts
        ret
.have_slot:
        mov     [mod_slot], bx
        ; ---- memory: the file, or more if the header asks ----
        mov     eax, [found_size]
        add     eax, 15
        shr     eax, 4
        mov     bx, [mod_hdr + MH_PARAS]
        cmp     bx, ax
        jae     .paras_ok
        mov     bx, ax
.paras_ok:
        cmp     bx, 0x1000
        ja      .bad_format
        call    mem_alloc_system                ; AX = the segment
        jc      .no_memory
        mov     [mod_seg], ax
        ; ---- load it, and zero the rest ----
        call    fstream_open
        mov     es, [mod_seg]
        xor     di, di
        mov     ecx, [found_size]
        call    fstream_read
        mov     es, [mod_seg]
        mov     di, [found_size]                ; 0 only for a 64 KB file
        or      di, di
        jz      .zeroed
        mov     cx, bx
        shl     cx, 4                           ; wraps to 0 for 4096 paragraphs:
        sub     cx, di                          ;  0 - size is still the right count
        xor     al, al
        rep     stosb
.zeroed:
        mov     word [es:MH_SERVICES], mod_services
        mov     word [es:MH_SERVICES + 2], cs
        ; ---- init ----
        mov     ax, [es:MH_INIT]
        mov     [mod_call], ax
        mov     [mod_call + 2], es
        mov     si, [mod_args]
        push    cs
        pop     es
        mov     ds, [mod_seg]
        call    far [cs:mod_call]
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        jc      .refused
        ; ---- keep what it wants kept ----
        mov     bx, [mod_hdr + MH_RESIDENT]
        or      bx, bx
        jz      .keep_all
        mov     es, [mod_seg]
        call    mem_resize                      ; smaller is always possible
        mov     ax, cs
        mov     es, ax
.keep_all:
        mov     bx, [mod_slot]
        mov     ax, [mod_seg]
        mov     [bx], ax
        clc
        ret
.refused:
        mov     es, [mod_seg]
        call    mem_free
        mov     ax, cs
        mov     es, ax
        mov     si, msg_mod_refused
        call    puts
        stc
        ret
.already:
        mov     si, msg_mod_already
        call    puts
        stc
        ret
.not_found:
        mov     si, msg_mod_not_found
        call    puts
        stc
        ret
.bad_format:
        mov     si, msg_mod_bad
        call    puts
        stc
        ret
.no_memory:
        mov     si, msg_no_memory
        call    puts
        stc
        ret

; -----------------------------------------------------------------------------
; mod_unload: DS:SI = name.  Only the module loaded last can go: an earlier
;   one may have vectors hooked behind it, and the chain would break.
; -----------------------------------------------------------------------------
mod_unload:
        ; the name as the table has it: 8 characters, upper case, padded
        mov     di, mod_name
        mov     cx, 8
.copy:  lodsb
        or      al, al
        jz      .pad
        cmp     al, ' '
        je      .pad
        call    upcase
        stosb
        loop    .copy
        jmp     .named
.pad:   mov     al, ' '
        rep     stosb
.named: mov     si, mod_name
        call    mod_find_name                   ; BX -> the entry
        jc      .not_loaded
        cmp     word [bx+2], 0                  ; another one after it?
        jne     .not_last
        mov     es, [bx]
        mov     ax, [es:MH_UNLOAD]
        or      ax, ax
        jz      .free
        mov     [mod_call], ax
        mov     [mod_call + 2], es
        push    bx
        mov     ds, [bx]
        call    far [cs:mod_call]
        pop     bx
        mov     ax, cs
        mov     ds, ax
        jc      .busy
        mov     es, [bx]
.free:  call    mem_free
        mov     word [bx], 0
        mov     ax, cs
        mov     es, ax
        mov     si, msg_mod_unloaded
        call    puts
        clc
        ret
.busy:  mov     ax, cs
        mov     es, ax
        mov     si, msg_mod_busy
        call    puts
        stc
        ret
.not_last:
        mov     si, msg_mod_not_last
        call    puts
        stc
        ret
.not_loaded:
        mov     si, msg_mod_not_loaded
        call    puts
        stc
        ret

; mod_find_name: DS:SI = 8-byte name -> BX = its table entry.  CF if none.
mod_find_name:
        push    ax
        push    cx
        push    di
        push    es
        mov     bx, mod_table
.next:  mov     ax, [bx]
        or      ax, ax
        jz      .none
        mov     es, ax
        push    si
        mov     di, MH_NAME
        mov     cx, 8
        repe    cmpsb
        pop     si
        je      .found
        add     bx, 2
        jmp     .next
.none:  stc
        jmp     .out
.found: clc
.out:   pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; mod_event: AL = event, told to every module that listens.  Keeps everything.
; -----------------------------------------------------------------------------
mod_event:
        pushad
        push    ds
        push    es
        mov     bx, mod_table
.next:  cmp     word [cs:bx], 0
        je      .done
        mov     es, [cs:bx]
        mov     dx, [es:MH_EVENT]
        or      dx, dx
        jz      .skip
        mov     [cs:mod_call], dx
        mov     [cs:mod_call + 2], es
        push    bx
        push    ax
        mov     ds, [cs:bx]
        call    far [cs:mod_call]
        pop     ax
        pop     bx
.skip:  add     bx, 2
        cmp     bx, mod_table + MOD_MAX * 2
        jb      .next
.done:  pop     es
        pop     ds
        popad
        ret

; =============================================================================
; the service table: what a module may ask of the kernel
; =============================================================================
; Each entry is a far pointer to a thunk.  A module far-calls it with DS = its
; own segment.  Thunks marked (kernel) switch DS to the kernel's for the
; routine and back; the others take their arguments where the routine does.
section .data
mod_services:
        dw MOD_SVC_COUNT
        dw svc_puts, KERNEL_SEG                 ; 0  DS:SI string
        dw svc_putc, KERNEL_SEG                 ; 1  AL
        dw svc_crlf, KERNEL_SEG                 ; 2
        dw svc_print_dec, KERNEL_SEG            ; 3  EAX
        dw svc_print_hex16, KERNEL_SEG          ; 4  AX
        dw svc_mem_alloc, KERNEL_SEG            ; 5  BX paragraphs -> AX segment, CF
        dw svc_mem_free, KERNEL_SEG             ; 6  ES = block
        dw svc_mem_resize, KERNEL_SEG           ; 7  ES = block, BX = paragraphs, CF
        dw svc_ext_mem_end, KERNEL_SEG          ; 8  -> EAX end of extended memory
        dw svc_log_text, KERNEL_SEG             ; 9  DS:SI line
        dw svc_log_line, KERNEL_SEG             ; 10 DS:SI text, EAX in hex
        dw svc_getkey, KERNEL_SEG               ; 11 -> AX
        dw svc_kbhit, KERNEL_SEG                ; 12 -> ZF clear when a key waits
        dw svc_delay_ticks, KERNEL_SEG          ; 13 CX ticks
        dw svc_mem_largest, KERNEL_SEG          ; 14 -> BX paragraphs
        dw svc_print_dec_pad, KERNEL_SEG        ; 15 EAX in a field of CL
MOD_SVC_COUNT   equ ($ - mod_services - 2) / 4
section .text

svc_puts:       call    puts
                retf
svc_putc:       call    putc
                retf
svc_crlf:       call    crlf
                retf
svc_print_dec:  call    print_dec
                retf
svc_print_hex16: call   print_hex16
                retf
svc_print_dec_pad: call print_dec_pad
                retf
svc_ext_mem_end: call   ext_mem_end
                retf
svc_getkey:     call    getkey
                retf
svc_kbhit:      call    kbhit
                retf
svc_delay_ticks: call   delay_ticks
                retf

; (kernel) thunks
svc_mem_alloc:
        push    ds
        push    cs
        pop     ds
        call    mem_alloc_system
        pop     ds
        retf
svc_mem_free:
        push    ds
        push    cs
        pop     ds
        call    mem_free
        pop     ds
        retf
svc_mem_resize:
        push    ds
        push    cs
        pop     ds
        call    mem_resize
        pop     ds
        retf
svc_mem_largest:
        push    ds
        push    cs
        pop     ds
        call    mem_largest
        pop     ds
        retf

; the log wants the kernel's DS, the string is in the module's: a byte at a time
svc_log_line:
        call    svc_log_copy
        push    ds
        push    cs
        pop     ds
        call    log_hex32
        call    log_crlf
        pop     ds
        retf
svc_log_text:
        call    svc_log_copy
        push    ds
        push    cs
        pop     ds
        call    log_crlf
        pop     ds
        retf
svc_log_copy:
        push    ax
        push    si
.next:  lodsb
        or      al, al
        jz      .done
        push    ds
        push    cs
        pop     ds
        call    log_putc
        pop     ds
        jmp     .next
.done:  pop     si
        pop     ax
        ret

section .data
msg_mod_head:       db "Loaded modules:", 13, 10, 0
msg_mod_none:       db "No modules are loaded.  LOAD <name> loads NAME.MOD.", 13, 10, 0
msg_mod_kb:         db " KB", 13, 10, 0
msg_mod_not_found:  db "Module not found", 13, 10, 0
msg_mod_bad:        db "Not a module file", 13, 10, 0
msg_mod_full:       db "No room for another module", 13, 10, 0
msg_mod_already:    db "That module is already loaded", 13, 10, 0
msg_mod_refused:    db "The module did not load", 13, 10, 0
msg_mod_not_loaded: db "No such module is loaded", 13, 10, 0
msg_mod_not_last:   db "Unload the modules loaded after it first", 13, 10, 0
msg_mod_busy:       db "The module cannot be unloaded now", 13, 10, 0
msg_mod_unloaded:   db "Module unloaded", 13, 10, 0
msg_mod_unload_usage: db "Usage: UNLOAD <name>", 13, 10, 0

section .bss
mod_table:      resw MOD_MAX                    ; segments, in load order
mod_hdr:        resb MH_SIZE
mod_name:       resb 8
mod_call:       resd 1
mod_seg:        resw 1
mod_slot:       resw 1
mod_args:       resw 1
section .text
