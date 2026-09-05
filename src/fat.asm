; =============================================================================
;  fat.asm - read-only FAT12 / FAT16 filesystem driver
; -----------------------------------------------------------------------------
;  Conventions: all routines preserve the registers they do not document as
;  outputs.  ES must equal DS (kernel data segment) when calling.
;
;  find_entry / resolve_path leave their result in found_cluster, found_size,
;  found_attr, found_date, found_time (and SI -> the raw 32-byte entry inside
;  dir_buf, valid until the next disk access).
; =============================================================================

ATTR_READONLY   equ 0x01
ATTR_HIDDEN     equ 0x02
ATTR_SYSTEM     equ 0x04
ATTR_VOLUME     equ 0x08
ATTR_DIRECTORY  equ 0x10
ATTR_ARCHIVE    equ 0x20
ATTR_LFN        equ 0x0F

; -----------------------------------------------------------------------------
; fs_init: read the volume boot record and derive the FAT layout
; -----------------------------------------------------------------------------
fs_init:
        pushad
        mov     byte [fs_ok], 0
        xor     eax, eax
        mov     bx, sector_buf
        call    read_sector
        jc      .done
        cmp     word [sector_buf+11], 512       ; bytes per sector
        jne     .done
        cmp     word [sector_buf+510], 0xAA55
        jne     .done
        mov     al, [sector_buf+13]             ; sectors per cluster
        or      al, al
        jz      .done
        mov     [fs_spc], al

        movzx   eax, word [sector_buf+14]       ; reserved sectors
        mov     [fs_fat_start], eax
        movzx   ecx, word [sector_buf+22]       ; sectors per FAT
        mov     [fs_fat_secs], ecx
        movzx   edx, byte [sector_buf+16]       ; number of FATs
        mov     [fs_num_fats], dl
        imul    ecx, edx
        add     eax, ecx
        mov     [fs_root_start], eax
        movzx   ecx, word [sector_buf+17]       ; root entries
        mov     [fs_root_entries], cx
        shl     ecx, 5                          ; * 32 bytes
        add     ecx, 511
        shr     ecx, 9                          ; -> sectors
        mov     [fs_root_secs], cx
        add     eax, ecx
        mov     [fs_data_start], eax

        movzx   ecx, word [sector_buf+19]       ; total sectors (16-bit)
        test    ecx, ecx
        jnz     .have_total
        mov     ecx, [sector_buf+32]            ; total sectors (32-bit)
.have_total:
        sub     ecx, eax                        ; data sectors
        mov     eax, ecx
        xor     edx, edx
        movzx   ebx, byte [fs_spc]
        div     ebx
        mov     [fs_total_clusters], eax
        mov     byte [fs_fat_type], 12
        cmp     eax, 4085
        jb      .type_done
        mov     byte [fs_fat_type], 16
.type_done:
        mov     si, sector_buf+43               ; volume label
        mov     di, fs_label
        mov     cx, 11
        rep     movsb
        mov     dword [fs_fat_cache_lba], -1
        mov     word [cur_dir_cluster], 0
        mov     byte [fs_ok], 1
.done:
        popad
        ret

; -----------------------------------------------------------------------------
; cluster_to_lba: AX = cluster -> EAX = LBA of its first sector
; -----------------------------------------------------------------------------
cluster_to_lba:
        push    ecx
        push    edx
        movzx   eax, ax
        sub     eax, 2
        movzx   ecx, byte [fs_spc]
        mul     ecx
        add     eax, [fs_data_start]
        pop     edx
        pop     ecx
        ret

; -----------------------------------------------------------------------------
; fat_entry: AX = cluster -> AX = raw FAT entry value.  CF=1 on disk error.
; -----------------------------------------------------------------------------
fat_entry:
        push    ebx
        push    ecx
        push    edx
        mov     dx, ax                          ; DX = cluster (parity)
        movzx   eax, ax
        mov     ebx, eax
        cmp     byte [fs_fat_type], 12
        jne     .fat16
        shr     ebx, 1
        add     ebx, eax                        ; offset = c + c/2
        jmp     .have_offset
.fat16:
        shl     ebx, 1                          ; offset = c * 2
.have_offset:
        mov     ecx, ebx
        shr     ecx, 9                          ; FAT sector index
        and     ebx, 511                        ; offset within sector
        add     ecx, [fs_fat_start]
        cmp     ecx, [fs_fat_cache_lba]
        je      .cached
        push    bx
        push    es
        mov     ax, ds
        mov     es, ax                          ; the cache lives in our segment
        mov     eax, ecx
        mov     bx, fat_buf
        call    read_sector
        jc      .error_pop
        inc     eax
        add     bx, 512
        call    read_sector                     ; second sector (entry may straddle)
        pop     es
        pop     bx
        mov     [fs_fat_cache_lba], ecx
.cached:
        mov     ax, [fat_buf+bx]
        cmp     byte [fs_fat_type], 12
        jne     .ok
        test    dl, 1
        jz      .even
        shr     ax, 4
        jmp     .ok
.even:  and     ax, 0x0FFF
.ok:    clc
        jmp     .ret
.error_pop:
        pop     es
        pop     bx
        stc
.ret:   pop     edx
        pop     ecx
        pop     ebx
        ret

; -----------------------------------------------------------------------------
; next_cluster: AX = cluster -> AX = next cluster.  CF=1 at end of chain
;   (or on a bad/free entry or disk error).
; -----------------------------------------------------------------------------
next_cluster:
        call    fat_entry
        jc      .end
        cmp     ax, 2
        jb      .end
        cmp     byte [fs_fat_type], 12
        jne     .f16
        cmp     ax, 0x0FF7
        jae     .end
        clc
        ret
.f16:   cmp     ax, 0xFFF7
        jae     .end
        clc
        ret
.end:   stc
        ret

; -----------------------------------------------------------------------------
; fs_free_clusters: EAX = number of free clusters (scans the FAT)
; -----------------------------------------------------------------------------
fs_free_clusters:
        push    ebx
        push    ecx
        mov     ecx, [fs_total_clusters]
        xor     ebx, ebx
        mov     ax, 2
.next:  push    ax
        call    fat_entry
        jc      .skip
        test    ax, ax
        jnz     .skip
        inc     ebx
.skip:  pop     ax
        inc     ax
        dec     ecx
        jnz     .next
        mov     eax, ebx
        pop     ecx
        pop     ebx
        ret

; -----------------------------------------------------------------------------
; dir_open: start iterating directory AX (0 = root directory)
; -----------------------------------------------------------------------------
dir_open:
        push    eax
        mov     [di_cluster], ax
        test    ax, ax
        jnz     .subdir
        mov     eax, [fs_root_start]
        mov     [di_lba], eax
        mov     ax, [fs_root_secs]
        mov     [di_secs_left], ax
        jmp     .done
.subdir:
        call    cluster_to_lba
        mov     [di_lba], eax
        movzx   ax, byte [fs_spc]
        mov     [di_secs_left], ax
.done:  mov     word [di_index], 16               ; force a sector load
        pop     eax
        ret

; -----------------------------------------------------------------------------
; dir_next: SI -> next raw 32-byte entry (in dir_buf).  CF=1 when finished.
;   Deleted / LFN / volume-label entries are NOT filtered here.
; -----------------------------------------------------------------------------
dir_next:
        push    eax
        push    bx
.again:
        cmp     word [di_index], 16
        jb      .have
        cmp     word [di_secs_left], 0
        jne     .load
        mov     ax, [di_cluster]
        test    ax, ax
        jz      .end                            ; root directory exhausted
        call    next_cluster
        jc      .end
        mov     [di_cluster], ax
        call    cluster_to_lba
        mov     [di_lba], eax
        movzx   ax, byte [fs_spc]
        mov     [di_secs_left], ax
.load:
        mov     eax, [di_lba]
        mov     bx, dir_buf
        push    es
        mov     ax, ds
        mov     es, ax
        mov     eax, [di_lba]
        call    read_sector
        pop     es
        jc      .end
        mov     eax, [di_lba]
        mov     [dir_buf_lba], eax
        inc     dword [di_lba]
        dec     word [di_secs_left]
        mov     word [di_index], 0
.have:
        mov     si, [di_index]
        shl     si, 5
        add     si, dir_buf
        inc     word [di_index]
        cmp     byte [si], 0                    ; 0x00 = no more entries
        je      .end
        pop     bx
        pop     eax
        clc
        ret
.end:   pop     bx
        pop     eax
        stc
        ret

; -----------------------------------------------------------------------------
; dir_next_visible: like dir_next but skips deleted, LFN and volume entries
; -----------------------------------------------------------------------------
dir_next_visible:
        push    ax
        call    lfn_begin
.again: call    dir_next
        jc      .done
        cmp     byte [si], 0xE5
        je      .restart                        ; a deleted entry ends any set
        mov     al, [si+11]
        cmp     al, ATTR_LFN
        je      .long_part
        test    al, ATTR_VOLUME
        jnz     .restart
        call    lfn_apply                       ; the name, if it checks out
        clc
.done:  pop     ax
        ret
.long_part:
        call    lfn_collect
        jmp     .again
.restart:
        call    lfn_begin
        jmp     .again

; -----------------------------------------------------------------------------
; find_entry: SI = 11-byte FAT name, AX = directory cluster (0 = root).
;   CF=0 and found_* filled if found (SI -> entry in dir_buf), else CF=1.
;   In the root directory "." and ".." resolve to the root itself.
; -----------------------------------------------------------------------------
find_entry:
        push    eax
        push    bx
        push    cx
        push    di
        mov     bx, si
        test    ax, ax
        jnz     .search
        cmp     byte [si], '.'
        jne     .search
        ; root "." / ".."
        mov     word [found_cluster], 0
        mov     byte [found_attr], ATTR_DIRECTORY
        mov     dword [found_size], 0
        mov     word [found_date], 0
        mov     word [found_time], 0
        mov     dword [found_dir_lba], 0
        clc
        jmp     .ret
.search:
        mov     word [match_len], 0
        call    dir_open
.next:  call    dir_next_visible
        jc      .notfound
        push    si
        mov     di, si
        mov     si, bx
        mov     cx, 11
        repe    cmpsb
        pop     si
        je      .hit
        ; not its 8.3 name: is it what the user typed, spelled out in full?
        cmp     word [match_text], 0
        je      .next
        push    si
        mov     si, [match_text]
        call    lfn_match                       ; CX = characters accounted for
        pop     si
        jc      .next
        mov     [match_len], cx
        mov     cx, [match_text]                ; where the name leaves off, so
        add     [match_len], cx                 ;  the caller can step over it
        mov     cx, [match_len]
        mov     [match_end], cx
        sub     cx, [match_text]
        mov     [match_len], cx
.hit:
        mov     ax, [si+26]
        mov     [found_cluster], ax
        mov     eax, [si+28]
        mov     [found_size], eax
        mov     al, [si+11]
        mov     [found_attr], al
        mov     ax, [si+24]
        mov     [found_date], ax
        mov     ax, [si+22]
        mov     [found_time], ax
        mov     ax, si                          ; where the entry itself is,
        sub     ax, dir_buf                     ;  so it can be rewritten
        mov     [found_dir_off], ax
        mov     eax, [dir_buf_lba]
        mov     [found_dir_lba], eax
        clc
        jmp     .ret
.notfound:
        stc
.ret:   mov     word [match_text], 0            ; never leaks to the next call
        pop     di
        pop     cx
        pop     bx
        pop     eax
        ret

; -----------------------------------------------------------------------------
; is_name_end: CF=1 if AL terminates a file name component
; -----------------------------------------------------------------------------
is_name_end:
        cmp     al, '"'                         ; closes a quoted name
        je      .yes
        cmp     al, ' '
        jne     .not_space
        cmp     byte [name_quoted], 0           ; inside quotes a space is
        je      .yes                            ;  part of the name
        clc
        ret
.not_space:
        cmp     al, 0
        je      .yes
        cmp     al, 9
        je      .yes
        cmp     al, 13
        je      .yes
        cmp     al, 10
        je      .yes
        cmp     al, '\'
        je      .yes
        cmp     al, '/'
        je      .yes
        clc
        ret
.yes:   stc
        ret

; -----------------------------------------------------------------------------
; to_fat_name: DS:SI = text ("hello.com"), DS:DI = 11-byte output.
;   Upper-cases and space-pads.  SI is advanced to the terminator.
; -----------------------------------------------------------------------------
to_fat_name:
        push    ax
        push    bx
        push    cx
        push    di
        mov     bx, di
        mov     cx, 11
        mov     al, ' '
        rep     stosb
        mov     di, bx
        cmp     byte [si], '.'
        jne     .name
        mov     byte [di], '.'
        inc     si
        cmp     byte [si], '.'
        jne     .finish
        mov     byte [di+1], '.'
        inc     si
        jmp     .finish
.name:  mov     cx, 8
.nloop: mov     al, [si]
        call    is_name_end
        jc      .finish
        inc     si
        cmp     al, '.'
        je      .ext
        cmp     al, '*'                         ; wildcard: rest of the field
        je      .star_name
        jcxz    .nloop                          ; more than 8 chars: drop extras
        call    upcase
        mov     [di], al
        inc     di
        dec     cx
        jmp     .nloop
.star_name:
        jcxz    .nloop
        mov     byte [di], '?'
        inc     di
        dec     cx
        jmp     .star_name
.ext:   lea     di, [bx+8]
        mov     cx, 3
.eloop: mov     al, [si]
        call    is_name_end
        jc      .finish
        inc     si
        cmp     al, '*'
        je      .star_ext
        jcxz    .eloop
        call    upcase
        mov     [di], al
        inc     di
        dec     cx
        jmp     .eloop
.star_ext:
        jcxz    .eloop
        mov     byte [di], '?'
        inc     di
        dec     cx
        jmp     .star_ext
.finish:
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; fat_name_to_str: SI = 11-byte name -> DI = "NAME.EXT" (NUL-terminated).
;   Returns CX = length.
; -----------------------------------------------------------------------------
fat_name_to_str:
        push    ax
        push    si
        push    di
        push    di
        mov     cx, 8
.n:     lodsb
        cmp     al, ' '
        je      .skipn
        stosb
.skipn: loop    .n
        cmp     byte [si], ' '
        je      .noext
        mov     al, '.'
        stosb
        mov     cx, 3
.e:     lodsb
        cmp     al, ' '
        je      .skipe
        stosb
.skipe: loop    .e
.noext: xor     al, al
        stosb
        pop     ax
        mov     cx, di
        sub     cx, ax
        dec     cx
        pop     di
        pop     si
        pop     ax
        ret

; -----------------------------------------------------------------------------
; resolve_path: DS:SI = path ("FILE.TXT", "DIR\FILE.TXT", "\DIR", "..").
;   Walks from the current directory (or root for a leading '\').
;   CF=0 and found_* describe the final component; CF=1 if not found.
;   SI is advanced past the path.
; -----------------------------------------------------------------------------
resolve_path:
        push    ax
        push    dx
        push    di
        cmp     byte [si+1], ':'                ; "C:" prefix: ignore the letter
        jne     .no_drive
        add     si, 2
.no_drive:
        mov     al, [path_api]                  ; a program's path: spaces count
        mov     [name_quoted], al
        cmp     byte [si], '"'                  ; "a name with spaces"
        jne     .no_quote
        inc     si
        mov     byte [name_quoted], 1
.no_quote:
        mov     ax, [cur_dir_cluster]
        mov     dl, [si]
        cmp     dl, '\'
        je      .absolute
        cmp     dl, '/'
        jne     .component
.absolute:
        inc     si
        xor     ax, ax
        mov     dl, [si]
        push    ax
        mov     al, dl
        call    is_name_end
        pop     ax
        jnc     .component
        ; the path was just "\" -> the root directory itself
        mov     word [found_cluster], 0
        mov     byte [found_attr], ATTR_DIRECTORY
        mov     dword [found_size], 0
        clc
        jmp     .ret
.component:
        mov     [match_text], si                ; what was actually typed
        mov     di, fat_name
        call    to_fat_name
        push    si
        mov     si, fat_name
        call    find_entry
        pop     si
        jc      .ret                            ; CF=1: not found
        cmp     word [match_len], 0             ; matched by its long name?
        je      .stepped
        mov     si, [match_end]                 ; step over all of it
        mov     word [match_len], 0
.stepped:
        mov     dl, [si]
        cmp     dl, '\'
        je      .more
        cmp     dl, '/'
        jne     .ok
.more:  inc     si
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .notfound
        mov     ax, [found_cluster]
        push    ax
        mov     al, [si]
        call    is_name_end
        pop     ax
        jc      .ok                             ; trailing separator
        jmp     .component
.ok:    clc
        jmp     .ret
.notfound:
        stc
.ret:   pushf
        cmp     byte [name_quoted], 0
        je      .unquoted
        cmp     byte [si], '"'
        jne     .unquoted
        inc     si                              ; step over the closing quote
.unquoted:
        mov     byte [name_quoted], 0
        popf
        pop     di
        pop     dx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; load_chain: AX = first cluster, ES:BX = destination, CX = max sectors.
;   Loads the cluster chain sector by sector.  CF=1 on disk error.
;   Returns DX = sectors loaded.
; -----------------------------------------------------------------------------
load_chain:
        push    eax
        push    bx
        push    cx
        push    si
        push    es
        xor     si, si                          ; SI = sectors loaded
        test    cx, cx
        jz      .ok
        cmp     ax, 2
        jb      .ok                             ; empty file
.cluster:
        push    ax
        call    cluster_to_lba
        movzx   dx, byte [fs_spc]
.sector:
        call    read_sector
        jc      .error_pop
        inc     si
        inc     eax
        add     bx, 512
        jnc     .no_wrap
        push    ax
        mov     ax, es
        add     ax, 0x1000
        mov     es, ax
        pop     ax
.no_wrap:
        dec     cx
        jz      .ok_pop
        dec     dx
        jnz     .sector
        pop     ax
        call    next_cluster
        jc      .ok
        jmp     .cluster
.ok_pop:
        pop     ax
.ok:    mov     dx, si
        clc
        jmp     .ret
.error_pop:
        pop     ax
        mov     dx, si
        stc
.ret:   pop     es
        pop     si
        pop     cx
        pop     bx
        pop     eax
        ret

; -----------------------------------------------------------------------------
; data
; -----------------------------------------------------------------------------
section .data
fs_ok:              db 0
fs_fat_type:        db 12
fs_spc:             db 1
                    align 4
fs_hidden:          dd 0
fs_fat_start:       dd 0
fs_root_start:      dd 0
fs_data_start:      dd 0
fs_total_clusters:  dd 0
fs_fat_cache_lba:   dd -1
fs_fat_secs:        dd 0
fs_num_fats:        db 0
fs_root_secs:       dw 0
fs_root_entries:    dw 0
cur_dir_cluster:    dw 0
cur_path:           db "\", 0
                    times 62 db 0
fs_label:           times 11 db ' '
                    db 0
found_cluster:      dw 0
found_size:         dd 0
found_attr:         db 0
found_date:         dw 0
found_time:         dw 0
found_dir_off:      dw 0
                    align 4
found_dir_lba:      dd 0
di_cluster:         dw 0
di_lba:             dd 0
di_secs_left:       dw 0
di_index:           dw 0
section .bss
sector_buf:         resb 512
fat_buf:            resb 1024
dir_buf:            resb 512
fat_name:           resb 12
match_text:         resw 1                      ; the text a name may spell out
match_len:          resw 1
match_end:          resw 1                      ; the text just past that name
name_quoted:        resb 1                      ; inside "quotes": spaces count
lfn_name:           resb LFN_MAX + 2            ; the long name of the last entry
name_str_long:      resb LFN_MAX + 2
lfn_sum:            resb 1
lfn_parts:          resb 1
lfn_want:           resb 1
lfn_bad:            resb 1
section .text
