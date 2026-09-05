; =============================================================================
;  fatwrite.asm - writing side of the FAT12 / FAT16 driver
; -----------------------------------------------------------------------------
;  Creating, extending and deleting files.  Every FAT update is written
;  through to all copies of the table immediately, and a file's directory
;  entry is rewritten when the handle is closed, so a stick pulled out
;  between operations loses at most the file being written.
;
;  Safety rules kept throughout: a cluster is only ever taken from the free
;  list, cluster numbers are range checked against the volume size before
;  they are turned into sector addresses, and nothing outside the data area
;  is ever written.
; =============================================================================

; -----------------------------------------------------------------------------
; fat_now: AX = FAT time of day, DX = FAT date
; -----------------------------------------------------------------------------
fat_now:
        push    bx
        push    cx
        call    rtc_read_time                   ; CH = hour, CL = min, DH = sec
        movzx   ax, ch
        shl     ax, 6
        movzx   bx, cl
        or      ax, bx
        shl     ax, 5
        movzx   bx, dh
        shr     bx, 1                           ; two-second resolution
        or      ax, bx
        push    ax
        call    rtc_read_date                   ; AX = year, DH = month, DL = day
        sub     ax, 1980
        cmp     ax, 127
        jbe     .year_ok
        xor     ax, ax
.year_ok:
        shl     ax, 4
        movzx   bx, dh
        or      ax, bx
        shl     ax, 5
        movzx   bx, dl
        or      ax, bx
        mov     dx, ax                          ; DX = date
        pop     ax                              ; AX = time
        pop     cx
        pop     bx
        ret

; -----------------------------------------------------------------------------
; cluster_valid: AX = cluster.  CF=0 if it addresses real data.
; -----------------------------------------------------------------------------
cluster_valid:
        push    eax
        cmp     ax, 2
        jb      .bad
        movzx   eax, ax
        sub     eax, 2
        cmp     eax, [fs_total_clusters]
        jae     .bad
        pop     eax
        clc
        ret
.bad:   pop     eax
        stc
        ret

; -----------------------------------------------------------------------------
; fat_set_entry: AX = cluster, DX = value.  Updates every copy of the FAT.
;   CF=1 on a disk error or an out-of-range cluster.
; -----------------------------------------------------------------------------
fat_set_entry:
        call    cluster_valid
        jc      .bad
        pushad
        push    es
        push    ax
        push    dx
        call    fat_entry                       ; brings the window into fat_buf
        pop     dx
        pop     ax
        jc      .fail
        mov     cx, ax                          ; CX = cluster (parity bit)
        movzx   eax, ax
        mov     ebx, eax
        cmp     byte [fs_fat_type], 12
        jne     .f16
        shr     ebx, 1
        add     ebx, eax                        ; offset = c + c/2
        jmp     .have_offset
.f16:   shl     ebx, 1                          ; offset = c * 2
.have_offset:
        mov     ebp, ebx
        shr     ebp, 9                          ; EBP = sector index within a FAT
        and     ebx, 511
        cmp     byte [fs_fat_type], 12
        jne     .set16
        mov     ax, [fat_buf+bx]
        test    cl, 1
        jz      .even
        and     ax, 0x000F                      ; odd cluster: high 12 bits
        mov     si, dx
        shl     si, 4
        or      ax, si
        jmp     .store
.even:  and     ax, 0xF000                      ; even cluster: low 12 bits
        mov     si, dx
        and     si, 0x0FFF
        or      ax, si
.store: mov     [fat_buf+bx], ax
        jmp     .write_back
.set16: mov     [fat_buf+bx], dx
.write_back:
        push    ds
        pop     es                              ; the buffer is in our segment
        movzx   ecx, byte [fs_num_fats]
        or      ecx, ecx
        jz      .fail
        mov     esi, [fs_fat_start]             ; ESI = start of the current FAT
.copy:  mov     eax, esi
        add     eax, ebp
        mov     bx, fat_buf
        call    write_sector
        jc      .fail
        mov     edx, ebp
        inc     edx
        cmp     edx, [fs_fat_secs]
        jae     .next_fat                       ; the window's second sector is
        inc     eax                             ;  past the end of this FAT
        mov     bx, fat_buf + 512
        call    write_sector
        jc      .fail
.next_fat:
        add     esi, [fs_fat_secs]
        dec     ecx
        jnz     .copy
        pop     es
        popad
        clc
        ret
.fail:  pop     es
        popad
.bad:   stc
        ret

; -----------------------------------------------------------------------------
; fat_alloc: take a free cluster, mark it end-of-chain.
;   AX = cluster, CF=1 if the volume is full.
; -----------------------------------------------------------------------------
fat_alloc:
        push    bx
        push    cx
        push    dx
        push    si
        mov     ecx, [fs_total_clusters]
        add     ecx, 2                          ; clusters are numbered 2..n+1
        cmp     ecx, 0xFFF0
        jbe     .limit_ok
        mov     ecx, 0xFFF0
.limit_ok:
        mov     si, [fs_alloc_hint]
        cmp     si, 2
        jae     .from_hint
        mov     si, 2
.from_hint:
        mov     bx, 2                           ; BX = passes remaining
.scan:  cmp     si, cx
        jb      .test
        mov     si, 2                           ; wrap to the start of the table
        dec     bx
        jz      .full
        jmp     .scan
.test:  mov     ax, si
        call    fat_entry
        jc      .full
        test    ax, ax
        jz      .found
        inc     si
        jmp     .scan
.found: mov     ax, si
        mov     dx, 0xFFFF                      ; end of chain
        call    fat_set_entry
        jc      .full
        mov     ax, si
        inc     si
        mov     [fs_alloc_hint], si
        clc
        jmp     .ret
.full:  stc
.ret:   pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; -----------------------------------------------------------------------------
; fat_free_chain: AX = first cluster of a chain; mark every cluster free.
; -----------------------------------------------------------------------------
fat_free_chain:
        pushad
        mov     si, ax
        mov     cx, 0xFFF0                      ; never loop forever on a bad FAT
.next:  or      si, si
        jz      .done
        mov     ax, si
        call    cluster_valid
        jc      .done
        mov     ax, si
        call    fat_entry                       ; remember where the chain goes
        jc      .done
        mov     di, ax                          ; DI = next cluster
        mov     ax, si
        xor     dx, dx
        call    fat_set_entry                   ; mark this one free
        jc      .done
        cmp     di, 2
        jb      .done
        cmp     byte [fs_fat_type], 12
        jne     .f16
        cmp     di, 0x0FF7
        jae     .done
        jmp     .step
.f16:   cmp     di, 0xFFF7
        jae     .done
.step:  mov     si, di
        dec     cx
        jnz     .next
.done:  popad
        ret

; -----------------------------------------------------------------------------
; dir_load: EAX = LBA of a directory sector -> dir_buf.  CF=1 on error.
; dir_store: write dir_buf back to the sector it came from.
; -----------------------------------------------------------------------------
dir_load:
        push    bx
        push    es
        push    ds
        pop     es
        mov     bx, dir_buf
        call    read_sector
        jc      .done
        mov     [dir_buf_lba], eax
.done:  pop     es
        pop     bx
        ret

dir_store:
        pushad
        push    es
        push    ds
        pop     es
        mov     eax, [dir_buf_lba]
        mov     bx, dir_buf
        call    write_sector
        pop     es
        popad
        ret

; -----------------------------------------------------------------------------
; dir_find_slot: AX = directory cluster (0 = root).  Finds a usable entry,
;   leaving its sector in dir_buf.  Returns BX = offset of the entry within
;   dir_buf.  CF=1 if the directory has no room left.
; -----------------------------------------------------------------------------
dir_find_slot:
        push    ax
        push    si
        call    dir_open
.loop:
        cmp     word [di_index], 16
        jb      .have
        cmp     word [di_secs_left], 0
        jne     .load
        mov     ax, [di_cluster]
        test    ax, ax
        jz      .full                           ; the root directory is fixed
        call    next_cluster
        jnc     .linked
        ; ---- no room left: give the directory another cluster ----
        call    fat_alloc
        jc      .full
        mov     [cf_grow], ax
        mov     dx, ax
        mov     ax, [di_cluster]
        call    fat_set_entry                   ; the old last cluster -> the new
        jc      .full
        mov     ax, [cf_grow]
        call    zero_cluster
        jc      .full
.linked:
        mov     [di_cluster], ax
        call    cluster_to_lba
        mov     [di_lba], eax
        movzx   ax, byte [fs_spc]
        mov     [di_secs_left], ax
.load:
        mov     eax, [di_lba]
        call    dir_load
        jc      .full
        inc     dword [di_lba]
        dec     word [di_secs_left]
        mov     word [di_index], 0
.have:
        mov     si, [di_index]
        shl     si, 5
        add     si, dir_buf
        inc     word [di_index]
        mov     al, [si]
        cmp     al, 0xE5                        ; a deleted entry: reuse it
        je      .found
        or      al, al                          ; never used: the end
        jz      .found
        jmp     .loop
.found: mov     bx, si
        sub     bx, dir_buf
        clc
        jmp     .ret
.full:  stc
.ret:   pop     si
        pop     ax
        ret

; -----------------------------------------------------------------------------
; path_parent: DS:SI = path -> AX = directory cluster holding the last
;   component, DI -> that component.  CF=1 if the path names no directory.
; -----------------------------------------------------------------------------
path_parent:
        push    bx
        push    dx
        cmp     byte [si+1], ':'
        jne     .no_drive
        add     si, 2
.no_drive:
        xor     bx, bx                          ; BX = last separator seen
        mov     di, si
.scan:  mov     dl, [di]
        or      dl, dl
        jz      .scanned
        cmp     dl, '\'
        je      .mark
        cmp     dl, '/'
        jne     .step
.mark:  mov     bx, di
.step:  inc     di
        jmp     .scan
.scanned:
        test    bx, bx
        jz      .in_cur_dir
        mov     di, bx
        inc     di                              ; DI -> the last component
        cmp     byte [di], 0
        je      .fail                           ; the path ends in a separator
        cmp     bx, si
        je      .in_root                        ; "\NAME"
        mov     dl, [bx]
        mov     byte [bx], 0                    ; cut the directory part out
        push    si
        push    di
        call    resolve_path
        pop     di
        pop     si
        mov     [bx], dl                        ; and put the path back together
        jc      .fail
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .fail
        mov     ax, [found_cluster]
        jmp     .ok
.in_root:
        xor     ax, ax
        jmp     .ok
.in_cur_dir:
        mov     ax, [cur_dir_cluster]
        mov     di, si
.ok:    clc
        pop     dx
        pop     bx
        ret
.fail:  stc
        pop     dx
        pop     bx
        ret

; -----------------------------------------------------------------------------
; fs_create: DS:SI = path.  Creates the file, or truncates it if it exists.
;   On success CF=0, [cf_lba] / [cf_off] locate its directory entry and
;   found_* describe the (now empty) file.  On failure CF=1, AX = DOS error.
; -----------------------------------------------------------------------------
fs_create:
        cmp     byte [fs_ok], 0
        je      .no_file
        push    si
        call    strip_drive
        jc      .no_file_pop
        call    path_parent                     ; AX = directory, DI = name
        jc      .no_file_pop
        mov     [cf_dir], ax
        push    si
        mov     si, di
        mov     di, cf_name
        call    to_fat_name
        pop     si
        cmp     byte [cf_name], ' '             ; an empty or bad name
        je      .no_file_pop
        push    si
        mov     si, cf_name
        mov     ax, [cf_dir]
        call    find_entry
        pop     si
        jc      .make_new
        ; ---- it exists: check we may replace it, then truncate ----
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     .denied_pop
        test    byte [found_attr], ATTR_READONLY
        jnz     .denied_pop
        mov     bx, [found_dir_off]
        mov     [cf_off], bx
        mov     eax, [found_dir_lba]
        mov     [cf_lba], eax
        or      eax, eax
        jz      .denied_pop                     ; not a real directory entry
        mov     ax, [found_cluster]
        or      ax, ax
        jz      .rewrite
        push    bx
        call    fat_free_chain
        pop     bx
        ; freeing reads the FAT, which leaves dir_buf alone, but be explicit
        mov     eax, [cf_lba]
        call    dir_load
        jc      .no_file_pop
.rewrite:
        mov     bx, [cf_off]
        add     bx, dir_buf
        jmp     .fill
.make_new:
        mov     ax, [cf_dir]
        call    dir_find_slot                   ; -> dir_buf, BX = offset
        jc      .full_pop
        mov     [cf_off], bx
        mov     eax, [dir_buf_lba]
        mov     [cf_lba], eax
        add     bx, dir_buf
        ; a fresh entry: name, then everything else cleared
        push    di
        push    cx
        mov     di, bx
        mov     si, cf_name
        mov     cx, 11
        rep     movsb
        mov     cx, 21
        xor     al, al
        rep     stosb                           ; attr..size all zero
        pop     cx
        pop     di
        pop     si
        push    si
.fill:
        ; BX -> the 32-byte entry inside dir_buf
        push    di
        mov     di, bx
        mov     si, cf_name
        mov     cx, 11
        rep     movsb                           ; name (unchanged when reusing)
        pop     di
        mov     byte [bx+11], ATTR_ARCHIVE
        mov     word [bx+26], 0                 ; first cluster: none yet
        mov     dword [bx+28], 0                ; size
        call    fat_now                         ; AX = time, DX = date
        mov     [bx+22], ax
        mov     [bx+24], dx
        mov     [bx+18], dx                     ; write date
        mov     [bx+16], dx                     ; creation date
        mov     [bx+14], ax                     ; creation time
        call    dir_store
        ; report the empty file, and where its entry is
        mov     ax, [cf_off]
        mov     [found_dir_off], ax
        mov     eax, [cf_lba]
        mov     [found_dir_lba], eax
        mov     word [found_cluster], 0
        mov     dword [found_size], 0
        mov     byte [found_attr], ATTR_ARCHIVE
        mov     [found_time], ax
        mov     [found_date], dx
        pop     si
        clc
        ret
.no_file_pop:
        pop     si
.no_file:
        mov     ax, 3                           ; path not found
        stc
        ret
.denied_pop:
        pop     si
        mov     ax, 5                           ; access denied
        stc
        ret
.full_pop:
        pop     si
        mov     ax, 4                           ; too many files in the directory
        stc
        ret

; -----------------------------------------------------------------------------
; fs_delete: DS:SI = path.  CF=1 with AX = DOS error on failure.
; -----------------------------------------------------------------------------
fs_delete:
        cmp     byte [fs_ok], 0
        je      .no_file
        push    si
        call    strip_drive
        jc      .no_file_pop
        call    resolve_path
        jc      .no_file_pop
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     .denied_pop
        test    byte [found_attr], ATTR_READONLY
        jnz     .denied_pop
        mov     bx, [found_dir_off]
        mov     [cf_off], bx
        mov     eax, [found_dir_lba]
        mov     [cf_lba], eax
        or      eax, eax
        jz      .denied_pop
        mov     ax, [found_cluster]
        or      ax, ax
        jz      .unlink
        call    fat_free_chain
.unlink:
        mov     eax, [cf_lba]
        call    dir_load
        jc      .no_file_pop
        mov     bx, [cf_off]
        mov     byte [dir_buf+bx], 0xE5
        call    dir_store
        pop     si
        clc
        ret
.no_file_pop:
        pop     si
.no_file:
        mov     ax, 2                           ; file not found
        stc
        ret
.denied_pop:
        pop     si
        mov     ax, 5
        stc
        ret

; -----------------------------------------------------------------------------
; fs_chain_cluster: BX = handle record, AX = wanted cluster index in the file.
;   Walks the chain, allocating and linking clusters when the file is too
;   short.  Returns the cluster in AX, CF=1 if the volume is full.
; -----------------------------------------------------------------------------
fs_chain_cluster:
        push    cx
        push    dx
        push    si
        mov     si, ax                          ; SI = wanted index
        mov     ax, [bx+H_START]
        or      ax, ax
        jnz     .have_first
        ; the file has no clusters at all yet
        call    fat_alloc
        jc      .full
        mov     [bx+H_START], ax
        mov     [bx+H_CUR], ax
        mov     word [bx+H_CUR_INDEX], 0
        or      byte [bx+H_FLAGS], HF_DIRTY
.have_first:
        mov     ax, [bx+H_CUR]
        call    cluster_valid
        jc      .restart
        cmp     si, [bx+H_CUR_INDEX]
        jae     .walk
.restart:
        mov     ax, [bx+H_START]
        mov     [bx+H_CUR], ax
        mov     word [bx+H_CUR_INDEX], 0
.walk:  mov     cx, [bx+H_CUR_INDEX]
        cmp     cx, si
        je      .done
        mov     ax, [bx+H_CUR]
        push    ax
        call    next_cluster
        jc      .extend
        pop     cx                              ; drop the saved cluster
        mov     [bx+H_CUR], ax
        inc     word [bx+H_CUR_INDEX]
        jmp     .walk
.extend:
        pop     cx                              ; CX = last cluster of the chain
        call    fat_alloc                       ; AX = the new one
        jc      .full
        mov     dx, ax
        push    dx
        mov     ax, cx
        call    fat_set_entry                   ; link it on
        pop     dx
        jc      .full
        mov     [bx+H_CUR], dx
        inc     word [bx+H_CUR_INDEX]
        or      byte [bx+H_FLAGS], HF_DIRTY
        jmp     .walk
.done:  mov     ax, [bx+H_CUR]
        call    cluster_valid
        jc      .full
        clc
        jmp     .ret
.full:  stc
.ret:   pop     si
        pop     dx
        pop     cx
        ret

; -----------------------------------------------------------------------------
; fs_commit_handle: BX = handle record.  Writes its directory entry back.
; -----------------------------------------------------------------------------
fs_commit_handle:
        pushad
        test    byte [bx+H_FLAGS], HF_DIRTY
        jz      .done
        mov     eax, [bx+H_DIR_LBA]
        or      eax, eax
        jz      .done
        call    dir_load
        jc      .done
        mov     si, [bx+H_DIR_OFF]
        add     si, dir_buf
        mov     ax, [bx+H_START]
        mov     [si+26], ax
        mov     eax, [bx+H_SIZE]
        mov     [si+28], eax
        call    fat_now                         ; AX = time, DX = date
        mov     [si+22], ax
        mov     [si+24], dx
        mov     [si+18], dx
        or      byte [si+11], ATTR_ARCHIVE
        call    dir_store
        and     byte [bx+H_FLAGS], ~HF_DIRTY
.done:  popad
        ret

section .data
cf_dir:         dw 0
cf_off:         dw 0
                align 4
cf_lba:         dd 0
dir_buf_lba:    dd 0
fs_alloc_hint:  dw 2
section .bss
cf_name:        resb 12
section .text
