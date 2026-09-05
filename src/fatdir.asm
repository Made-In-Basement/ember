; =============================================================================
;  fatdir.asm - making, removing and renaming: INT 21h 39h, 3Ah and 56h
; -----------------------------------------------------------------------------
;  Built on the write layer in fatwrite.asm.  A new directory gets one
;  cluster holding its "." and ".." entries; removing one needs it empty;
;  renaming moves the 32-byte entry to its new place (which may be another
;  directory for a file) and marks the old one deleted, so no data moves.
; =============================================================================

; -----------------------------------------------------------------------------
; zero_cluster: AX = cluster.  Fills it with zero bytes.  CF=1 on a disk error.
; -----------------------------------------------------------------------------
zero_cluster:
        pushad
        push    es
        push    ds
        pop     es
        push    ax
        mov     di, sector_buf
        mov     cx, 512
        xor     al, al
        rep     stosb
        pop     ax
        call    cluster_to_lba                  ; EAX = first sector
        movzx   cx, byte [fs_spc]
        mov     bx, sector_buf
.next:  call    write_sector
        jc      .done
        inc     eax
        loop    .next
        clc
.done:  pop     es
        popad
        ret

; -----------------------------------------------------------------------------
; fs_mkdir: DS:SI = path.  CF=1 with AX = DOS error on failure.
; -----------------------------------------------------------------------------
fs_mkdir:
        cmp     byte [fs_ok], 0
        je      .no_path
        push    si
        push    es
        push    ds
        pop     es
        call    strip_drive
        jc      .bad_path
        push    si
        call    path_parent                     ; AX = parent, DI = the name
        pop     si
        jc      .bad_path
        mov     [cf_dir], ax
        mov     si, di
        mov     di, cf_name
        call    to_fat_name
        cmp     byte [cf_name], ' '
        je      .bad_path
        mov     si, cf_name
        mov     ax, [cf_dir]
        call    find_entry
        jnc     .exists
        mov     ax, [cf_dir]
        call    dir_find_slot                   ; the parent's sector in dir_buf
        jc      .full
        mov     [cf_off], bx
        mov     eax, [dir_buf_lba]
        mov     [cf_lba], eax
        call    fat_alloc                       ; AX = the new directory's cluster
        jc      .disk_full
        mov     [cf_cluster], ax
        call    zero_cluster
        jc      .disk_error
        ; ---- "." and ".." in its first sector ----
        mov     di, sector_buf
        mov     cx, 512
        xor     al, al
        rep     stosb
        mov     di, sector_buf
        mov     al, ' '
        mov     cx, 11
        rep     stosb
        mov     di, sector_buf + 32
        mov     cx, 11
        rep     stosb
        mov     byte [sector_buf], '.'
        mov     word [sector_buf + 32], ".."
        mov     byte [sector_buf + 11], ATTR_DIRECTORY
        mov     byte [sector_buf + 32 + 11], ATTR_DIRECTORY
        mov     ax, [cf_cluster]
        mov     [sector_buf + 26], ax
        mov     ax, [cf_dir]
        mov     [sector_buf + 32 + 26], ax      ; 0 when the parent is the root
        call    fat_now                         ; AX = time, DX = date
        mov     [sector_buf + 22], ax
        mov     [sector_buf + 24], dx
        mov     [sector_buf + 32 + 22], ax
        mov     [sector_buf + 32 + 24], dx
        push    ax
        push    dx
        mov     ax, [cf_cluster]
        call    cluster_to_lba
        mov     bx, sector_buf
        call    write_sector
        pop     dx
        pop     ax
        jc      .disk_error
        ; ---- its entry in the parent, still in dir_buf ----
        mov     bx, [cf_off]
        add     bx, dir_buf
        mov     di, bx
        mov     si, cf_name
        mov     cx, 11
        rep     movsb
        mov     cx, 21
        push    ax
        xor     al, al
        rep     stosb                           ; the rest of the entry cleared
        pop     ax
        mov     byte [bx + 11], ATTR_DIRECTORY
        mov     [bx + 22], ax
        mov     [bx + 24], dx
        mov     [bx + 16], dx
        mov     [bx + 14], ax
        mov     cx, [cf_cluster]
        mov     [bx + 26], cx
        call    dir_store
        pop     es
        pop     si
        clc
        ret
.bad_path:
        mov     ax, 3                           ; path not found
        jmp     .fail
.exists:
        mov     ax, 5                           ; access denied
        jmp     .fail
.full:  mov     ax, 4                           ; the parent has no room
        jmp     .fail
.disk_full:
        mov     ax, 8                           ; DOS says "insufficient memory"
        jmp     .fail
.disk_error:
        mov     ax, 5
.fail:  pop     es
        pop     si
        stc
        ret
.no_path:
        mov     ax, 3
        stc
        ret

; -----------------------------------------------------------------------------
; fs_rmdir: DS:SI = path.  The directory must be empty and not the current
;   one.  CF=1 with AX = DOS error on failure.
; -----------------------------------------------------------------------------
fs_rmdir:
        cmp     byte [fs_ok], 0
        je      .no_path
        push    si
        call    strip_drive
        jc      .bad_path
        call    resolve_path
        jc      .bad_path
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .bad_path
        mov     ax, [found_cluster]
        or      ax, ax
        jz      .denied                         ; the root
        cmp     ax, [cur_dir_cluster]
        je      .current
        cmp     dword [found_dir_lba], 0
        je      .denied
        mov     [cf_cluster], ax
        mov     bx, [found_dir_off]
        mov     [cf_off], bx
        mov     eax, [found_dir_lba]
        mov     [cf_lba], eax
        ; ---- anything inside besides "." and ".."? ----
        mov     ax, [cf_cluster]
        call    dir_open
.scan:  call    dir_next
        jc      .empty
        mov     al, [si]
        or      al, al
        jz      .empty
        cmp     al, 0xE5
        je      .scan
        cmp     al, '.'
        je      .scan
        cmp     byte [si + 11], ATTR_LFN
        je      .scan
        jmp     .not_empty
.empty: mov     ax, [cf_cluster]
        call    fat_free_chain
        mov     eax, [cf_lba]
        call    dir_load
        jc      .bad_path
        mov     bx, [cf_off]
        mov     byte [dir_buf + bx], 0xE5
        call    dir_store
        pop     si
        clc
        ret
.bad_path:
        mov     ax, 3
        jmp     .fail
.denied:
.not_empty:
        mov     ax, 5
        jmp     .fail
.current:
        mov     ax, 16                          ; the current directory
.fail:  pop     si
        stc
        ret
.no_path:
        mov     ax, 3
        stc
        ret

; -----------------------------------------------------------------------------
; fs_rename: DS:SI = the old path, DS:DI = the new one.  A file may move to
;   another directory; a directory may only change its name in place.
;   CF=1 with AX = DOS error on failure.
; -----------------------------------------------------------------------------
fs_rename:
        cmp     byte [fs_ok], 0
        je      .no_path
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     [cf_newp], di
        call    strip_drive
        jc      .no_file
        push    si
        call    path_parent                     ; AX = where the old one lives
        pop     si
        jc      .no_file
        mov     [cf_old_dir], ax
        call    resolve_path
        jc      .no_file
        cmp     dword [found_dir_lba], 0
        je      .denied
        ; ---- keep a copy of the old entry ----
        mov     eax, [found_dir_lba]
        mov     [cf_lba], eax
        mov     bx, [found_dir_off]
        mov     [cf_off], bx
        call    dir_load
        jc      .no_file
        mov     si, dir_buf
        add     si, bx
        mov     di, cf_entry
        mov     cx, 32
        rep     movsb
        ; ---- where it is going ----
        mov     si, [cf_newp]
        call    strip_drive
        jc      .no_file
        push    si
        call    path_parent                     ; AX = new directory, DI = name
        pop     si
        jc      .no_file
        mov     [cf_dir], ax
        test    byte [cf_entry + 11], ATTR_DIRECTORY
        jz      .placed
        cmp     ax, [cf_old_dir]                ; a directory stays where it is
        jne     .denied
.placed:
        mov     si, di
        mov     di, cf_name
        call    to_fat_name
        cmp     byte [cf_name], ' '
        je      .no_file
        mov     si, cf_name
        mov     ax, [cf_dir]
        call    find_entry
        jnc     .exists
        mov     ax, [cf_dir]
        call    dir_find_slot                   ; -> dir_buf, BX = offset
        jc      .full
        mov     di, dir_buf
        add     di, bx
        mov     si, cf_entry
        mov     cx, 32
        rep     movsb
        mov     di, dir_buf
        add     di, bx
        mov     si, cf_name
        mov     cx, 11
        rep     movsb
        call    dir_store
        ; ---- and the old entry goes (its sector may be the one just written) ----
        mov     eax, [cf_lba]
        call    dir_load
        jc      .no_file
        mov     bx, [cf_off]
        mov     byte [dir_buf + bx], 0xE5
        call    dir_store
        pop     es
        pop     di
        pop     si
        clc
        ret
.no_file:
        mov     ax, 2
        jmp     .fail
.exists:
.denied:
        mov     ax, 5
        jmp     .fail
.full:  mov     ax, 4
.fail:  pop     es
        pop     di
        pop     si
        stc
        ret
.no_path:
        mov     ax, 3
        stc
        ret

cf_cluster:     dw 0
cf_old_dir:     dw 0
cf_newp:        dw 0
cf_grow:        dw 0
cf_entry:       times 32 db 0
