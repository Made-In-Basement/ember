; =============================================================================
;  fatlfn.asm - long file names
; -----------------------------------------------------------------------------
;  A file whose name does not fit 8.3 carries extra directory entries in
;  front of its real one, each holding 13 characters of the long name and
;  marked with an attribute (0Fh) that DOS was written to ignore.  That is
;  how Windows put long names on a FAT disk without breaking DOS, and it is
;  why a disk written on a modern machine still works here.
;
;  dir_next_visible gathers those entries as it skips them, so by the time
;  it hands back a real entry, lfn_name holds that file's long name (or is
;  empty when it has none, or when the set does not check out).  DOS
;  programs still see only the 8.3 name: nothing about INT 21h changes.
;
;  Names longer than LFN_MAX characters are left to their short name.
; =============================================================================

LFN_PARTS       equ 6                           ; entries we will follow
LFN_MAX         equ LFN_PARTS * 13              ; 78 characters

; -----------------------------------------------------------------------------
; lfn_begin: forget any part-collected name (a new entry starts here)
; -----------------------------------------------------------------------------
lfn_begin:
        push    ax
        mov     byte [lfn_name], 0
        mov     byte [lfn_bad], 1               ; nothing in progress
        mov     byte [lfn_parts], 0
        mov     byte [lfn_want], 0
        pop     ax
        ret

; -----------------------------------------------------------------------------
; lfn_collect: SI -> a long-name entry in dir_buf.  Files its characters.
; -----------------------------------------------------------------------------
lfn_collect:
        pusha
        mov     al, [si]
        test    al, 0x40                        ; the set starts here
        jz      .continuing
        and     al, 0x3F
        mov     [lfn_want], al
        mov     byte [lfn_parts], 0
        mov     byte [lfn_bad], 0
        mov     al, [si+13]
        mov     [lfn_sum], al
        cmp     byte [lfn_want], 1
        jb      .give_up
        cmp     byte [lfn_want], LFN_PARTS
        ja      .give_up                        ; longer than we care to hold
        ; clear the whole buffer so a short set leaves no tail behind
        push    di
        mov     di, lfn_name
        mov     cx, LFN_MAX + 2
        xor     al, al
        rep     stosb
        pop     di
.continuing:
        cmp     byte [lfn_bad], 0
        jne     .done
        mov     al, [si+13]
        cmp     al, [lfn_sum]                   ; all of a set agree on this
        jne     .give_up
        mov     al, [si]
        and     al, 0x3F
        or      al, al
        jz      .give_up
        cmp     al, [lfn_want]
        ja      .give_up
        ; the 13 characters of part AL go at (AL-1) * 13
        dec     al
        mov     bl, 13
        mul     bl                              ; AX = offset
        mov     di, lfn_name
        add     di, ax
        mov     bx, lfn_offsets
        mov     cx, 13
.char:  movzx   ax, byte [bx]
        push    bx
        mov     bx, si
        add     bx, ax
        mov     ax, [bx]                        ; one UCS-2 character
        pop     bx
        cmp     ax, 0xFFFF                      ; padding after the end
        je      .store_nul
        or      ax, ax
        jz      .store_nul
        cmp     ax, 0x7F                        ; outside plain ASCII
        jb      .store
        mov     al, '_'
.store: mov     [di], al
        jmp     .next_char
.store_nul:
        mov     byte [di], 0
.next_char:
        inc     di
        inc     bx
        loop    .char
        ; remember we have this part
        mov     al, [si]
        and     al, 0x3F
        dec     al
        mov     cl, al
        mov     al, 1
        shl     al, cl
        or      [lfn_parts], al
        jmp     .done
.give_up:
        mov     byte [lfn_bad], 1
        mov     byte [lfn_name], 0
.done:  popa
        ret

; where the characters sit inside a long-name entry
lfn_offsets:    db 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30

; -----------------------------------------------------------------------------
; lfn_apply: SI -> the real entry that the collected name belongs to.
;   Keeps the name only if every part arrived and the checksum matches.
; -----------------------------------------------------------------------------
lfn_apply:
        pusha
        cmp     byte [lfn_bad], 0
        jne     .drop
        ; every part from 1 to want must have been seen
        mov     cl, [lfn_want]
        or      cl, cl
        jz      .drop
        mov     al, 1
        shl     al, cl
        dec     al                              ; the bits we wanted
        cmp     al, [lfn_parts]
        jne     .drop
        call    lfn_checksum                    ; AL = checksum of SI's 8.3 name
        cmp     al, [lfn_sum]
        jne     .drop
        cmp     byte [lfn_name], 0              ; an empty name is no name
        je      .drop
        popa
        ret
.drop:  mov     byte [lfn_name], 0
        popa
        ret

; -----------------------------------------------------------------------------
; lfn_checksum: SI -> an 11-byte 8.3 name -> AL.  This is the sum the long
;   entries carry, and it is what ties them to their own short entry.
; -----------------------------------------------------------------------------
lfn_checksum:
        push    cx
        push    si
        xor     al, al
        mov     cx, 11
.next:  ror     al, 1
        add     al, [si]
        inc     si
        loop    .next
        pop     si
        pop     cx
        ret

; -----------------------------------------------------------------------------
; lfn_match: DS:SI = typed text.  Compares it against the long name of the
;   entry just walked over, ignoring case.  CF=0 and CX = how many
;   characters of the text the name accounted for; CF=1 if it is not this
;   file.  The text must end there: at a separator or the end of the string.
; -----------------------------------------------------------------------------
lfn_match:
        push    ax
        push    bx
        push    dx
        push    si
        cmp     byte [lfn_name], 0
        je      .no
        mov     bx, lfn_name
        xor     cx, cx
.next:  mov     al, [bx]
        or      al, al
        jz      .name_done
        mov     dl, [si]
        or      dl, dl
        jz      .no
        cmp     dl, '\'
        je      .no
        cmp     dl, '/'
        je      .no
        cmp     dl, '"'
        je      .no
        call    upcase                          ; AL
        xchg    al, dl
        call    upcase                          ; DL, through AL
        xchg    al, dl
        cmp     al, dl
        jne     .no
        inc     bx
        inc     si
        inc     cx
        jmp     .next
.name_done:
        ; the typed text has to end here too
        mov     al, [si]
        or      al, al
        jz      .yes
        cmp     al, '\'
        je      .yes
        cmp     al, '/'
        je      .yes
        cmp     al, '"'
        je      .yes
.no:    pop     si
        pop     dx
        pop     bx
        pop     ax
        stc
        ret
.yes:   pop     si
        pop     dx
        pop     bx
        pop     ax
        clc
        ret

; -----------------------------------------------------------------------------
; entry_name: SI -> a directory entry -> DI = a NUL-terminated name to show,
;   long if there is one, otherwise the 8.3 name.  CX = its length.
;   The name lives in name_str either way.
; -----------------------------------------------------------------------------
entry_name:
        push    si
        cmp     byte [lfn_name], 0
        je      .short_name
        mov     si, lfn_name
        mov     di, name_str_long
        xor     cx, cx
.copy:  lodsb
        mov     [di], al
        inc     di
        or      al, al
        jz      .copied
        inc     cx
        jmp     .copy
.copied:
        mov     di, name_str_long
        pop     si
        ret
.short_name:
        mov     di, name_str_long
        call    fat_name_to_str                 ; SI's 11 bytes -> DI, CX = length
        mov     di, name_str_long
        pop     si
        ret
