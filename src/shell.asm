; =============================================================================
;  shell.asm - the command interpreter (COMMAND.COM equivalent)
; =============================================================================

CMDLINE_MAX     equ 127
COPY_CHUNK      equ 512

; -----------------------------------------------------------------------------
; shell_main: the read-eval loop.  Never returns.
; -----------------------------------------------------------------------------
shell_main:
.loop:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     sp, 0xFFFE                      ; fresh stack every command
        cmp     byte [after_pending], 0         ; a program left orders
        je      .prompt
        call    take_after
        call    run_batch_lines
        jmp     .loop
.prompt:
        call    print_prompt
        mov     di, cmdline
        mov     cx, CMDLINE_MAX
        mov     word [readline_tab], shell_complete
        call    readline
        mov     word [readline_tab], 0
        call    crlf
        mov     si, cmdline
        call    execute_line
        jmp     .loop

; -----------------------------------------------------------------------------
; print_prompt: "C:\PATH>"
; -----------------------------------------------------------------------------
print_prompt:
        push    ax
        push    si
        mov     al, [drive_letter]
        call    putc
        mov     al, ':'
        call    putc
        mov     si, cur_path
        call    puts
        mov     al, '>'
        call    putc
        pop     si
        pop     ax
        ret

; -----------------------------------------------------------------------------
; execute_line: DS:SI = NUL-terminated command line.  Clobbers registers.
; -----------------------------------------------------------------------------
execute_line:
        mov     ax, cs
        mov     es, ax
        mov     byte [echo_dot], 0
        call    skip_spaces
        cmp     byte [si], 0
        je      .done
        ; ---- copy the command word (upper-cased) into cmd_word ----
        mov     di, cmd_word
        xor     cx, cx
.copy:  mov     al, [si]
        or      al, al
        jz      .copied
        cmp     al, ' '
        je      .copied
        cmp     al, 9
        je      .copied
        ; "CD.." / "CD\" and "ECHO." are accepted without a space
        cmp     cx, 2
        jne     .not_cd
        cmp     word [cmd_word], "CD"
        jne     .not_cd
        cmp     al, '.'
        je      .copied
        cmp     al, '\'
        je      .copied
        cmp     al, '/'
        je      .copied
.not_cd:
        cmp     cx, 4
        jne     .not_echo
        cmp     dword [cmd_word], "ECHO"
        jne     .not_echo
        cmp     al, '.'
        jne     .not_echo
        mov     byte [echo_dot], 1
        inc     si
        jmp     .copied
.not_echo:
        cmp     cx, 12
        jae     .skip_char
        call    upcase
        stosb
        inc     cx
.skip_char:
        inc     si
        jmp     .copy
.copied:
        mov     byte [di], 0
        call    skip_spaces
        mov     [args_ptr], si
        ; ---- look the word up in the built-in table ----
        mov     bx, command_table
.lookup:
        cmp     byte [bx], 0
        je      .not_builtin
        mov     di, bx
        mov     si, cmd_word
        call    strcmp
        je      .found
.adv:   inc     bx                              ; skip name
        cmp     byte [bx-1], 0
        jne     .adv
        add     bx, 2                           ; skip handler
        jmp     .lookup
.found:
        mov     di, bx
.fh:    inc     di
        cmp     byte [di-1], 0
        jne     .fh
        mov     si, [args_ptr]
        call    word [di]
        jmp     .done
.not_builtin:
        call    run_program
.done:  mov     ax, cs
        mov     ds, ax
        mov     es, ax
        ret

; -----------------------------------------------------------------------------
; run_program: cmd_word names a .COM or .BAT file (extension optional)
; -----------------------------------------------------------------------------
run_program:
        cmp     byte [fs_ok], 0
        je      .bad
        mov     si, cmd_word
        mov     di, path_buf
        xor     bl, bl                          ; BL = last component has a dot
.copy:  lodsb
        cmp     al, '\'
        je      .sep
        cmp     al, '/'
        je      .sep
        cmp     al, '.'
        jne     .store
        mov     bl, 1
        jmp     .store
.sep:   xor     bl, bl
.store: stosb
        or      al, al
        jnz     .copy
        dec     di                              ; DI -> NUL terminator
        test    bl, bl
        jnz     .as_is
        mov     dword [di], ".COM"
        mov     byte [di+4], 0
        mov     si, path_buf
        call    resolve_path
        jnc     .check
        mov     dword [di], ".EXE"
        mov     si, path_buf
        call    resolve_path
        jnc     .check
        mov     dword [di], ".BAT"
        mov     si, path_buf
        call    resolve_path
        jnc     .check
        mov     dword [di], ".N32"
        mov     si, path_buf
        call    resolve_path
        jnc     .check
        jmp     .bad
.as_is: mov     si, path_buf
        call    resolve_path
        jc      .bad
.check: test    byte [found_attr], ATTR_DIRECTORY
        jnz     .bad
        cmp     word [fat_name+8], "BA"
        jne     .run
        cmp     byte [fat_name+10], 'T'
        jne     .run
        call    run_batch
        ret
.run:   mov     si, path_buf
        mov     di, [args_ptr]
        call    run_program_file                ; CF=0 if the program ran
        jnc     .ran                            ; program ran and exited
        mov     si, msg_no_memory
        cmp     ax, 8
        je      .print
        mov     si, msg_bad_format
        cmp     ax, 11
        je      .print
        push    ax
        mov     si, msg_exec_error
        call    puts
        pop     ax
        movzx   eax, ax
        call    print_dec
        call    crlf
        ret
.print: call    puts
.ran:   ret
.bad:   mov     si, msg_bad_command
        call    puts
        ret

; -----------------------------------------------------------------------------
; run_batch: execute the batch file described by found_*
; -----------------------------------------------------------------------------
run_batch:
        cmp     byte [batch_active], 0
        jne     .nested
        cmp     dword [found_size], BATCH_MAX
        ja      .too_large
        mov     ax, ds
        mov     es, ax
        mov     bx, batch_buf
        mov     ax, [found_cluster]
        mov     cx, BATCH_MAX / 512 + 1
        call    load_chain
        jc      .disk_error
        mov     bx, [found_size]
        mov     byte [batch_buf+bx], 0
        mov     byte [batch_active], 1
        mov     byte [batch_end], 0
        mov     word [batch_pos], 0
        mov     byte [batch_echo], 1
        jmp     run_batch_lines
.nested:
        mov     si, msg_batch_nested
        call    puts
        ret
.too_large:
        mov     si, msg_batch_large
        call    puts
        ret
.disk_error:
        mov     ax, ds
        mov     es, ax
        mov     si, msg_disk_error
        call    puts
        ret

; take_after: what a program asked to have run after it (see fF4) becomes
;   the batch in progress, quietly, whatever batch was running before
take_after:
        push    si
        push    di
        push    cx
        mov     si, after_buf
        mov     di, batch_buf
        mov     cx, AFTER_MAX
        rep     movsb
        mov     byte [after_pending], 0
        mov     byte [after_buf], 0
        mov     byte [batch_active], 1
        mov     byte [batch_end], 0
        mov     word [batch_pos], 0
        mov     byte [batch_echo], 0
        pop     cx
        pop     di
        pop     si
        ret

; run_batch_lines: execute batch_buf from batch_pos on
run_batch_lines:
.line:
        mov     si, [batch_pos]
        add     si, batch_buf
        mov     di, cmdline
        xor     cx, cx
.ch:    mov     al, [si]
        or      al, al
        jz      .last_line
        inc     si
        cmp     al, 10
        je      .eol
        cmp     al, 13
        je      .ch
        cmp     cx, CMDLINE_MAX
        jae     .ch
        mov     [di], al
        inc     di
        inc     cx
        jmp     .ch
.last_line:
        mov     byte [batch_end], 1
.eol:   sub     si, batch_buf
        mov     [batch_pos], si
        mov     byte [di], 0
        mov     si, cmdline
        call    skip_spaces
        cmp     byte [si], 0
        je      .next
        cmp     byte [si], ':'                  ; label
        je      .next
        cmp     byte [si], '@'
        jne     .maybe_echo
        inc     si
        jmp     .exec
.maybe_echo:
        cmp     byte [batch_echo], 0
        je      .exec
        call    crlf
        call    print_prompt
        call    puts
        call    crlf
.exec:  call    execute_line
.next:  cmp     byte [after_pending], 0         ; the program that just ran left orders
        je      .go_on
        call    take_after
        jmp     .line
.go_on: cmp     byte [batch_end], 0
        je      .line
        mov     byte [batch_active], 0
        ret

; fF4: INT 21h AH=F4h - run these lines (DS:DX, NUL-terminated, CR/LF
;   between them) after the calling program ends.  A desktop uses it to
;   start a DOS program and be started again afterwards.
fF4:    push    es
        push    ds
        push    si
        push    di
        push    cx
        mov     ax, cs
        mov     es, ax
        mov     ds, R_DS
        mov     si, R_DX
        mov     di, after_buf
        mov     cx, AFTER_MAX - 1
.copy:  lodsb
        stosb
        or      al, al
        jz      .copied
        loop    .copy
        mov     byte [es:di], 0
.copied:
        mov     byte [es:after_pending], 1
        pop     cx
        pop     di
        pop     si
        pop     ds
        pop     es
        mov     R_AX, 0
        ret

; -----------------------------------------------------------------------------
; run_autoexec: run \AUTOEXEC.BAT if it exists
; -----------------------------------------------------------------------------
run_autoexec:
        cmp     byte [fs_ok], 0
        je      .done
        mov     si, autoexec_name
        call    resolve_path
        jc      .done
        call    run_batch
.done:  ret

; =============================================================================
;  Built-in commands.  Each is called with DS:SI -> arguments.
; =============================================================================

; ---- HELP -------------------------------------------------------------------
cmd_help:
        mov     si, msg_help
        call    puts
        ret

; ---- VER --------------------------------------------------------------------
cmd_ver:
        mov     si, msg_version
        call    puts
        ret

; ---- CLS --------------------------------------------------------------------
cmd_cls:
        call    cls
        ret

; ---- ECHO -------------------------------------------------------------------
cmd_echo:
        cmp     byte [echo_dot], 1
        je      .newline
        cmp     byte [si], 0
        je      .status
        ; ECHO OFF / ECHO ON
        mov     al, [si]
        call    upcase
        cmp     al, 'O'
        jne     .print
        mov     al, [si+1]
        call    upcase
        cmp     al, 'F'
        je      .off
        cmp     al, 'N'
        jne     .print
        cmp     byte [si+2], 0
        jne     .print
        mov     byte [batch_echo], 1
        ret
.off:   mov     al, [si+2]
        call    upcase
        cmp     al, 'F'
        jne     .print
        cmp     byte [si+3], 0
        jne     .print
        mov     byte [batch_echo], 0
        ret
.print: call    puts
.newline:
        call    crlf
        ret
.status:
        mov     si, msg_echo_on
        cmp     byte [batch_echo], 0
        jne     .s
        mov     si, msg_echo_off
.s:     call    puts
        ret

; ---- DIR --------------------------------------------------------------------
cmd_dir:
        cmp     byte [fs_ok], 0
        je      no_filesystem
        mov     ax, [cur_dir_cluster]
        cmp     byte [si], 0
        je      .have_dir
        call    resolve_path
        jc      .not_found
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .not_found
        mov     ax, [found_cluster]
.have_dir:
        mov     [dir_cluster], ax
        push    si
        mov     si, msg_volume
        call    puts
        mov     al, [drive_letter]
        call    putc
        mov     si, msg_volume2
        call    puts
        mov     si, fs_label
        call    puts
        call    crlf
        mov     si, msg_directory
        call    puts
        mov     al, [drive_letter]
        call    putc
        mov     al, ':'
        call    putc
        pop     si
        cmp     byte [si], 0
        jne     .print_arg
        mov     si, cur_path
        call    puts
        jmp     .header_done
.print_arg:
        mov     ax, si
        call    puts
.header_done:
        call    crlf
        call    crlf
        mov     word [dir_files], 0
        mov     word [dir_dirs], 0
        mov     dword [dir_bytes], 0
        mov     ax, [dir_cluster]
        call    dir_open
.next:  call    dir_next_visible
        jc      .summary
        mov     cx, 8
        call    puts_n
        mov     al, ' '
        call    putc
        add     si, 8
        mov     cx, 3
        call    puts_n
        sub     si, 8
        test    byte [si+11], ATTR_DIRECTORY
        jz      .file
        push    si
        mov     si, msg_dir_tag
        call    puts
        pop     si
        inc     word [dir_dirs]
        jmp     .stamp
.file:  mov     eax, [si+28]
        mov     cl, 12
        call    print_dec_pad
        add     [dir_bytes], eax
        inc     word [dir_files]
.stamp: mov     cx, 2
        call    put_spaces
        mov     ax, [si+24]
        call    print_fat_date
        mov     cx, 2
        call    put_spaces
        mov     ax, [si+22]
        call    print_fat_time
        ; a long name, when the file has one, after its 8.3 columns
        cmp     byte [lfn_name], 0
        je      .no_long
        push    si
        mov     cx, 2
        call    put_spaces
        mov     si, lfn_name
        call    puts
        pop     si
.no_long:
        call    crlf
        ; allow ESC to abort long listings
        call    kbhit
        jz      .next
        call    getkey
        cmp     al, 27
        jne     .next
        ret
.summary:
        movzx   eax, word [dir_files]
        mov     cl, 9
        call    print_dec_pad
        mov     si, msg_files
        call    puts
        mov     eax, [dir_bytes]
        mov     cl, 12
        call    print_dec_pad
        mov     si, msg_bytes
        call    puts
        movzx   eax, word [dir_dirs]
        mov     cl, 9
        call    print_dec_pad
        mov     si, msg_dirs
        call    puts
        call    fs_free_clusters
        movzx   ecx, byte [fs_spc]
        shl     ecx, 9
        mul     ecx
        mov     cl, 12
        call    print_dec_pad
        mov     si, msg_bytes_free
        call    puts
        ret
.not_found:
        mov     si, msg_path_not_found
        call    puts
        ret

; print_fat_date: AX = FAT date word -> "MM-DD-YYYY"
print_fat_date:
        push    ax
        push    bx
        mov     bx, ax
        shr     ax, 5
        and     al, 0x0F
        call    print_2d
        mov     al, '-'
        call    putc
        mov     ax, bx
        and     al, 0x1F
        call    print_2d
        mov     al, '-'
        call    putc
        mov     ax, bx
        shr     ax, 9
        add     ax, 1980
        push    eax
        movzx   eax, ax
        call    print_dec
        pop     eax
        pop     bx
        pop     ax
        ret

; print_fat_time: AX = FAT time word -> "HH:MMa" (12-hour clock)
print_fat_time:
        push    ax
        push    bx
        push    dx
        mov     bx, ax
        shr     ax, 11                          ; hours 0..23
        mov     dl, 'a'
        cmp     al, 12
        jb      .am
        mov     dl, 'p'
        sub     al, 12
.am:    or      al, al
        jnz     .h
        mov     al, 12
.h:     call    print_2d_sp
        mov     al, ':'
        call    putc
        mov     ax, bx
        shr     ax, 5
        and     al, 0x3F
        call    print_2d
        mov     al, dl
        call    putc
        pop     dx
        pop     bx
        pop     ax
        ret

no_filesystem:
        mov     si, msg_no_fs
        call    puts
        ret

; ---- CD / CHDIR ---------------------------------------------------------------
cmd_cd:
        cmp     byte [fs_ok], 0
        je      no_filesystem
        cmp     byte [si], 0
        jne     .change
        mov     al, [drive_letter]
        call    putc
        mov     al, ':'
        call    putc
        mov     si, cur_path
        call    puts
        call    crlf
        ret
.change:
        call    change_dir
        jnc     .done
        mov     si, msg_invalid_dir
        cmp     al, 2
        jne     .print
        mov     si, msg_path_too_long
.print: call    puts
.done:  ret

; change_dir: DS:SI = path -> CF=1 on failure (AL = 1 invalid, 2 too long)
change_dir:
        cmp     byte [si+1], ':'                ; drive letter: skip it
        jne     .no_drive
        add     si, 2
.no_drive:
        mov     byte [name_quoted], 0
        cmp     byte [si], '"'                  ; "a directory with spaces"
        jne     .no_quote
        inc     si
        mov     byte [name_quoted], 1
.no_quote:
        ; work on a scratch copy so a failed CD leaves things untouched
        mov     ax, [cur_dir_cluster]
        mov     [tmp_cluster], ax
        push    si
        mov     si, cur_path
        mov     di, tmp_path
        mov     cx, 64
        rep     movsb
        pop     si
        mov     al, [si]
        cmp     al, '\'
        je      .root
        cmp     al, '/'
        jne     .component
.root:  inc     si
        mov     word [tmp_cluster], 0
        mov     word [tmp_path], 0x005C         ; "\", NUL
.component:
        mov     al, [si]
        or      al, al
        jz      .commit
        cmp     al, '"'
        je      .commit
        cmp     al, ' '
        jne     .not_space
        cmp     byte [name_quoted], 0
        je      .commit                         ; unquoted: the path ends here
.not_space:
        mov     [match_text], si                ; what was actually typed
        mov     di, fat_name
        call    to_fat_name
        cmp     byte [fat_name], '.'
        jne     .normal
        cmp     byte [fat_name+1], '.'
        jne     .separator                      ; "." -> stay
        ; ".." -> parent
        cmp     word [tmp_cluster], 0
        je      .separator                      ; already in the root
        push    si
        mov     si, fat_name
        mov     ax, [tmp_cluster]
        call    find_entry
        pop     si
        jc      .invalid
        mov     ax, [found_cluster]
        mov     [tmp_cluster], ax
        call    path_strip_last
        jmp     .separator
.normal:
        push    si
        mov     si, fat_name
        mov     ax, [tmp_cluster]
        call    find_entry
        pop     si
        jc      .invalid
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .invalid
        cmp     word [match_len], 0             ; matched by its long name?
        je      .stepped
        mov     si, [match_end]                 ; step over all of it
        mov     word [match_len], 0
.stepped:
        mov     ax, [found_cluster]
        mov     [tmp_cluster], ax
        call    path_append
        jc      .too_long
.separator:
        mov     al, [si]
        cmp     al, '\'
        je      .skip_sep
        cmp     al, '/'
        jne     .component
.skip_sep:
        inc     si
        jmp     .component
.commit:
        mov     byte [name_quoted], 0
        mov     ax, [tmp_cluster]
        mov     [cur_dir_cluster], ax
        mov     si, tmp_path
        mov     di, cur_path
        mov     cx, 64
        rep     movsb
        clc
        ret
.invalid:
        mov     al, 1
        stc
        ret
.too_long:
        mov     al, 2
        stc
        ret

; path_strip_last: remove the last "\NAME" from tmp_path
path_strip_last:
        push    ax
        push    di
        mov     di, tmp_path
        xor     ax, ax                          ; AX = position of last backslash
.scan:  cmp     byte [di], 0
        je      .end
        cmp     byte [di], '\'
        jne     .n
        mov     ax, di
.n:     inc     di
        jmp     .scan
.end:   cmp     ax, tmp_path
        jne     .cut
        inc     ax                              ; keep the root backslash
.cut:   mov     di, ax
        mov     byte [di], 0
        pop     di
        pop     ax
        ret

; path_append: append "\" + fat_name (as NAME.EXT) to tmp_path.  CF=1 if too long
path_append:
        push    ax
        push    cx
        push    si
        push    di
        cmp     byte [lfn_name], 0              ; the name as the user knows it
        je      .short_name
        mov     si, lfn_name
        mov     di, name_str
        xor     cx, cx
.copy_long:
        lodsb
        mov     [di], al
        inc     di
        or      al, al
        jz      .have_name
        inc     cx
        jmp     .copy_long
.short_name:
        mov     si, fat_name
        mov     di, name_str
        call    fat_name_to_str                 ; CX = length
.have_name:
        mov     di, tmp_path
.find_end:
        cmp     byte [di], 0
        je      .at_end
        inc     di
        jmp     .find_end
.at_end:
        mov     ax, di
        sub     ax, tmp_path
        add     ax, cx
        cmp     ax, 62
        jae     .too_long
        cmp     byte [di-1], '\'
        je      .copy
        mov     byte [di], '\'
        inc     di
.copy:  mov     si, name_str
        inc     cx
        rep     movsb                           ; includes NUL
        clc
        jmp     .done
.too_long:
        stc
.done:  pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---- TYPE -------------------------------------------------------------------
cmd_type:
        cmp     byte [fs_ok], 0
        je      no_filesystem
        cmp     byte [si], 0
        je      .missing
        call    resolve_path
        jc      .not_found
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     .not_found
        mov     eax, [found_size]
        mov     [type_remaining], eax
        mov     ax, [found_cluster]
        cmp     ax, 2
        jb      .done
.cluster:
        push    ax
        call    cluster_to_lba
        movzx   dx, byte [fs_spc]
.sector:
        cmp     dword [type_remaining], 0
        je      .done_pop
        mov     bx, sector_buf
        call    read_sector
        jc      .disk_error_pop
        mov     ecx, [type_remaining]
        cmp     ecx, 512
        jbe     .count_ok
        mov     ecx, 512
.count_ok:
        sub     [type_remaining], ecx
        mov     si, sector_buf
.char:  mov     bl, [si]
        inc     si
        cmp     bl, 10
        je      .lf
        cmp     bl, 13
        je      .next_char
        cmp     bl, 26
        je      .done_pop                       ; Ctrl-Z: end of text
        cmp     bl, 9
        je      .tab
        push    ax
        mov     al, bl
        call    putc
        pop     ax
        jmp     .next_char
.lf:    call    crlf
        call    kbhit
        jz      .next_char
        push    ax
        call    getkey
        cmp     al, 27
        pop     ax
        je      .done_pop
        jmp     .next_char
.tab:   push    ax
        push    bx
        push    cx
        push    dx
        mov     ah, 0x03
        xor     bh, bh
        int     0x10                            ; DL = column
        mov     cx, 8
        and     dl, 7
        sub     cl, dl
        call    put_spaces
        pop     dx
        pop     cx
        pop     bx
        pop     ax
.next_char:
        loop    .char
        inc     eax
        dec     dx
        jnz     .sector
        pop     ax
        call    next_cluster
        jc      .done
        jmp     .cluster
.done_pop:
        pop     ax
.done:  ret
.disk_error_pop:
        pop     ax
        mov     si, msg_disk_error
        call    puts
        ret
.missing:
        mov     si, msg_missing_param
        call    puts
        ret
.not_found:
        mov     si, msg_file_not_found
        call    puts
        ret

; ---- TIME -------------------------------------------------------------------
cmd_time:
        mov     si, msg_time
        call    puts
        call    rtc_read_time
        mov     al, ch
        call    print_2d
        mov     al, ':'
        call    putc
        mov     al, cl
        call    print_2d
        mov     al, ':'
        call    putc
        mov     al, dh
        call    print_2d
        call    crlf
        ret

; ---- DATE -------------------------------------------------------------------
cmd_date:
        mov     si, msg_date
        call    puts
        call    rtc_read_date                   ; AX = year, DH = month, DL = day
        push    ax
        push    dx
        call    day_of_week
        movzx   si, al
        shl     si, 2
        add     si, day_names
        call    puts
        mov     al, ' '
        call    putc
        pop     dx
        pop     ax
        push    ax
        mov     al, dh
        call    print_2d
        mov     al, '-'
        call    putc
        mov     al, dl
        call    print_2d
        mov     al, '-'
        call    putc
        pop     ax
        movzx   eax, ax
        call    print_dec
        call    crlf
        ret

; ---- MEM --------------------------------------------------------------------
cmd_mem:
        int     0x12                            ; AX = KB of conventional memory
        movzx   eax, ax
        mov     cl, 10
        call    print_dec_pad
        mov     si, msg_mem_conv
        call    puts
        ; extended memory
        xor     ecx, ecx
        xor     edx, edx
        mov     ax, 0xE801
        int     0x15
        jc      .try_88
        or      ax, ax
        jnz     .use_ax
        mov     ax, cx
        mov     bx, dx
.use_ax:
        movzx   eax, ax
        movzx   ebx, bx
        shl     ebx, 6                          ; 64 KB blocks -> KB
        add     eax, ebx
        jmp     .show_ext
.try_88:
        mov     ah, 0x88
        int     0x15
        jc      .no_ext
        movzx   eax, ax
        jmp     .show_ext
.no_ext:
        xor     eax, eax
.show_ext:
        mov     cl, 10
        call    print_dec_pad
        mov     si, msg_mem_ext
        call    puts
        mov     eax, bss_end
        add     eax, 1023
        shr     eax, 10
        mov     cl, 10
        call    print_dec_pad
        mov     si, msg_mem_kernel
        call    puts
        call    mem_free_total
        shr     eax, 10
        mov     cl, 10
        call    print_dec_pad
        mov     si, msg_mem_free
        call    puts
        ret

; ---- COLOR ------------------------------------------------------------------
cmd_color:
        cmp     byte [si], 0
        je      .reset
        call    parse_hex_byte                  ; AL = value, CF=1 if invalid
        jc      .usage
        mov     [screen_attr], al
        call    cls
        ret
.reset: mov     byte [screen_attr], 0x07
        call    cls
        ret
.usage: mov     si, msg_color_usage
        call    puts
        ret

; parse_hex_byte: DS:SI -> 1-2 hex digits -> AL.  CF=1 if invalid.
parse_hex_byte:
        push    bx
        push    cx
        xor     bl, bl
        xor     cx, cx
.next:  mov     al, [si]
        call    is_name_end
        jc      .end
        call    upcase
        sub     al, '0'
        jb      .bad
        cmp     al, 9
        jbe     .digit
        sub     al, 7
        cmp     al, 10
        jb      .bad
        cmp     al, 15
        ja      .bad
.digit: shl     bl, 4
        or      bl, al
        inc     si
        inc     cx
        cmp     cx, 2
        jbe     .next
        jmp     .bad
.end:   test    cx, cx
        jz      .bad
        mov     al, bl
        clc
        jmp     .done
.bad:   stc
.done:  pop     cx
        pop     bx
        ret

; ---- BEEP -------------------------------------------------------------------
cmd_beep:
        call    beep
        ret

; beep: sound the PC speaker for ~150 ms at ~880 Hz
beep:
        push    ax
        push    cx
        push    dx
        mov     al, 0xB6
        out     0x43, al                        ; PIT channel 2, square wave
        mov     ax, 1193182 / 880
        out     0x42, al
        mov     al, ah
        out     0x42, al
        in      al, 0x61
        or      al, 0x03                        ; gate + speaker enable
        out     0x61, al
        mov     cx, 3                           ; about 165 ms
        call    delay_ticks
        in      al, 0x61
        and     al, 0xFC
        out     0x61, al
        pop     dx
        pop     cx
        pop     ax
        ret

; ---- PAUSE ------------------------------------------------------------------
cmd_pause:
        mov     si, msg_pause
        call    puts
        call    getkey
        call    crlf
        ret

; ---- REM --------------------------------------------------------------------
cmd_rem:
        ret

; ---- REBOOT -----------------------------------------------------------------
cmd_reboot:
        mov     si, msg_rebooting
        call    puts
        mov     cx, 9
        call    delay_ticks
        mov     al, 0xFE
        out     0x64, al                        ; keyboard controller reset line
        mov     cx, 18
        call    delay_ticks
        jmp     0xFFFF:0x0000                   ; fall back to the BIOS entry

; ---- SHUTDOWN ---------------------------------------------------------------
cmd_shutdown:
        mov     byte [screen_attr], 0x0E
        call    cls
        mov     si, msg_shutdown
        call    puts
        mov     cx, 18
        call    delay_ticks
        ; APM: connect, set version 1.2, power off
        mov     ax, 0x5300
        xor     bx, bx
        int     0x15
        jc      .no_apm
        mov     ax, 0x5301
        xor     bx, bx
        int     0x15
        mov     ax, 0x530E
        xor     bx, bx
        mov     cx, 0x0102
        int     0x15
        mov     ax, 0x5307
        mov     bx, 0x0001
        mov     cx, 0x0003
        int     0x15
.no_apm:
        mov     si, msg_safe_off
        call    puts
.halt:  cli
        hlt
        jmp     .halt

; ---- TRACE: toggle logging of every INT 21h call into EMBER.LOG -------------
cmd_trace:
        xor     byte [trace_flag], 1
        mov     si, msg_trace_on
        cmp     byte [trace_flag], 0
        jne     .say
        mov     si, msg_trace_off
.say:   call    puts
        ret

; ---- WIN: start the graphical shell -----------------------------------------
cmd_win:
        call    gui_main
        ret

; =============================================================================
; command table: name, NUL, handler
; =============================================================================
command_table:
        db "HELP", 0
        dw cmd_help
        db "?", 0
        dw cmd_help
        db "VER", 0
        dw cmd_ver
        db "CLS", 0
        dw cmd_cls
        db "DIR", 0
        dw cmd_dir
        db "CD", 0
        dw cmd_cd
        db "CHDIR", 0
        dw cmd_cd
        db "TYPE", 0
        dw cmd_type
        db "ECHO", 0
        dw cmd_echo
        db "TIME", 0
        dw cmd_time
        db "DATE", 0
        dw cmd_date
        db "MEM", 0
        dw cmd_mem
        db "COLOR", 0
        dw cmd_color
        db "BEEP", 0
        dw cmd_beep
        db "PAUSE", 0
        dw cmd_pause
        db "REM", 0
        dw cmd_rem
        db "REBOOT", 0
        dw cmd_reboot
        db "SHUTDOWN", 0
        dw cmd_shutdown
        db "WIN", 0
        dw cmd_win
        db "PLAY", 0
        dw cmd_play
        db "SPEAKER", 0
        dw cmd_speaker
        db "SPLASH", 0
        dw cmd_splash
        db "MKDIR", 0
        dw cmd_mkdir
        db "MD", 0
        dw cmd_mkdir
        db "RMDIR", 0
        dw cmd_rmdir
        db "RD", 0
        dw cmd_rmdir
        db "REN", 0
        dw cmd_ren
        db "RENAME", 0
        dw cmd_ren
        db "COPY", 0
        dw cmd_copy
        db "DEL", 0
        dw cmd_del
        db "ERASE", 0
        dw cmd_del
        db "SOUND", 0
        dw cmd_sound
        db "TRACE", 0
        dw cmd_trace
        db 0

; =============================================================================
; -----------------------------------------------------------------------------
; cmd_copy: COPY source destination
; -----------------------------------------------------------------------------
cmd_copy:
        call    next_arg                        ; SI -> first argument
        jc      .usage
        mov     di, copy_src
        call    copy_arg
        call    next_arg
        jc      .usage
        mov     di, copy_dst
        call    copy_arg
        ; open the source
        mov     dx, copy_src
        mov     ax, 0x3D00
        int     0x21
        jc      .no_source
        mov     [copy_in], ax
        ; create the destination
        mov     dx, copy_dst
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .no_dest
        mov     [copy_out], ax
        xor     eax, eax
        mov     [copy_total], eax
.chunk: mov     bx, [copy_in]
        mov     cx, COPY_CHUNK
        mov     dx, copy_buf
        mov     ah, 0x3F
        int     0x21
        jc      .read_error
        or      ax, ax
        jz      .finished
        mov     cx, ax
        movzx   eax, ax
        add     [copy_total], eax
        mov     bx, [copy_out]
        mov     dx, copy_buf
        mov     ah, 0x40
        int     0x21
        jc      .write_error
        cmp     ax, cx
        jne     .write_error
        cmp     cx, COPY_CHUNK
        je      .chunk
.finished:
        call    .close_both
        mov     si, msg_copy_done
        call    puts
        mov     eax, [copy_total]
        call    print_dec
        mov     si, msg_copy_bytes
        call    puts
        ret
.read_error:
        call    .close_both
        mov     si, msg_copy_read
        call    puts
        ret
.write_error:
        call    .close_both
        mov     si, msg_copy_write
        call    puts
        ret
.close_both:
        mov     bx, [copy_in]
        mov     ah, 0x3E
        int     0x21
        mov     bx, [copy_out]
        mov     ah, 0x3E
        int     0x21
        ret
.no_dest:
        mov     bx, [copy_in]
        mov     ah, 0x3E
        int     0x21
        mov     si, msg_copy_write
        call    puts
        ret
.no_source:
        mov     si, msg_copy_read
        call    puts
        ret
.usage: mov     si, msg_copy_usage
        call    puts
        ret

; -----------------------------------------------------------------------------
; cmd_del: DEL name
; -----------------------------------------------------------------------------
cmd_del:
        call    next_arg
        jc      .usage
        mov     di, copy_src
        call    copy_arg
        mov     dx, copy_src
        mov     ah, 0x41
        int     0x21
        jc      .failed
        mov     si, msg_del_done
        call    puts
        ret
.failed:
        mov     si, msg_del_failed
        call    puts
        ret
.usage: mov     si, msg_del_usage
        call    puts
        ret

; cmd_mkdir / cmd_rmdir / cmd_ren: the directory commands, through INT 21h
cmd_mkdir:
        call    next_arg
        jc      .usage
        mov     di, copy_src
        call    copy_arg
        mov     dx, copy_src
        mov     ah, 0x39
        int     0x21
        jc      .failed
        ret
.failed:
        mov     si, msg_mkdir_failed
        call    puts
        ret
.usage: mov     si, msg_mkdir_usage
        call    puts
        ret

cmd_rmdir:
        call    next_arg
        jc      .usage
        mov     di, copy_src
        call    copy_arg
        mov     dx, copy_src
        mov     ah, 0x3A
        int     0x21
        jc      .failed
        ret
.failed:
        mov     si, msg_rmdir_failed
        call    puts
        ret
.usage: mov     si, msg_rmdir_usage
        call    puts
        ret

cmd_ren:
        call    next_arg
        jc      .usage
        mov     di, copy_src
        call    copy_arg
        call    next_arg
        jc      .usage
        mov     di, copy_dst
        call    copy_arg
        push    ds
        pop     es
        mov     dx, copy_src
        mov     di, copy_dst
        mov     ah, 0x56
        int     0x21
        jc      .failed
        ret
.failed:
        mov     si, msg_ren_failed
        call    puts
        ret
.usage: mov     si, msg_ren_usage
        call    puts
        ret

; next_arg: SI -> command tail; skip spaces.  CF=1 if there is nothing left.
next_arg:
        push    ax
.skip:  mov     al, [si]
        cmp     al, ' '
        je      .adv
        cmp     al, 9
        jne     .check
.adv:   inc     si
        jmp     .skip
.check: or      al, al
        jz      .none
        cmp     al, 13
        je      .none
        pop     ax
        clc
        ret
.none:  pop     ax
        stc
        ret

; copy_arg: copy the word at SI to DI, NUL terminated; SI ends past it
copy_arg:
        push    ax
        push    cx
        mov     cx, 78
        cmp     byte [si], '"'                  ; "a name with spaces.txt"
        jne     .next
        inc     si
.quoted:
        mov     al, [si]
        or      al, al
        jz      .done
        cmp     al, 13
        je      .done
        inc     si
        cmp     al, '"'
        je      .done
        mov     [di], al
        inc     di
        loop    .quoted
        jmp     .done
.next:  mov     al, [si]
        or      al, al
        jz      .done
        cmp     al, ' '
        je      .done
        cmp     al, 9
        je      .done
        cmp     al, 13
        je      .done
        mov     [di], al
        inc     si
        inc     di
        loop    .next
.done:  mov     byte [di], 0
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; cmd_speaker: SPEAKER [ON|OFF] - play PC speaker sound through the sound chip
; -----------------------------------------------------------------------------
cmd_speaker:
        call    next_arg
        jc      .status
        mov     di, copy_src
        call    copy_arg
        mov     si, copy_src
        call    upcase_str
        cmp     word [copy_src], "ON"
        jne     .try_off
        cmp     byte [copy_src+2], 0
        jne     .try_off
        mov     byte [spk_want], 1
        call    spk_bridge_start
        jnc     .started
        mov     byte [spk_want], 0
        mov     si, msg_spk_none
        cmp     ax, 2
        jne     .say
        mov     si, msg_spk_notrap
.say:   call    puts
        ret
.started:
        mov     si, msg_spk_on
        call    puts
        ret
.try_off:
        cmp     word [copy_src], "OF"
        jne     .status
        mov     byte [spk_want], 0
        call    spk_bridge_stop
        mov     si, msg_spk_off
        call    puts
        ret
.no_sound:
        mov     byte [spk_want], 0
        mov     si, msg_spk_none
        call    puts
        ret
.status:
        mov     si, msg_spk_off
        cmp     byte [spk_on], 0
        je      .show
        mov     si, msg_spk_on
.show:  call    puts
        mov     si, msg_spk_ports
        call    puts
        mov     eax, [spk_w42]
        call    print_dec
        mov     al, '/'
        call    putc
        mov     eax, [spk_w43]
        call    print_dec
        mov     al, '/'
        call    putc
        mov     eax, [spk_w61]
        call    print_dec
        mov     si, msg_spk_seen
        call    puts
        mov     eax, [spk_traps]
        call    print_dec
        mov     si, msg_spk_gate
        call    puts
        mov     eax, [spk_refills]
        call    print_dec
        call    crlf
        mov     si, msg_spk_note
        call    puts
        movzx   eax, byte [spk_mode]
        call    print_dec
        mov     si, msg_spk_div
        call    puts
        movzx   eax, word [spk_reload]
        call    print_dec
        mov     si, msg_spk_gatenow
        call    puts
        movzx   eax, byte [spk_gate]
        call    print_dec
        call    crlf
        mov     si, msg_spk_cost
        call    puts
        mov     eax, [spk_worst]
        call    print_dec
        mov     si, msg_spk_each
        call    puts
        mov     eax, [spk_per_trap]
        call    print_dec
        mov     si, msg_spk_cycles
        call    puts
        cmp     byte [spk_dropped61], 0
        je      .no_drop61
        mov     si, msg_spk_drop
        call    puts
.no_drop61:
        cmp     byte [spk_dropped43], 0
        je      .no_drop
        mov     si, msg_spk_drop43
        call    puts
.no_drop:
        cmp     byte [spk_panicked], 0
        je      .no_panic
        mov     si, msg_spk_panic
        call    puts
.no_panic:
        mov     si, msg_spk_usage
        call    puts
        ret

; upcase_str: upper-case the NUL-terminated string at DS:SI
upcase_str:
        push    ax
        push    si
.next:  mov     al, [si]
        or      al, al
        jz      .done
        call    upcase
        mov     [si], al
        inc     si
        jmp     .next
.done:  pop     si
        pop     ax
        ret

section .data
msg_spk_on:     db "PC speaker sound is played through the sound chip.", 13, 10, 0
msg_spk_off:    db "PC speaker sound goes to the speaker line only.", 13, 10, 0
msg_spk_none:   db "No HD Audio hardware to play it through.", 13, 10, 0
msg_spk_notrap: db "This processor does not trap port I/O, so the bridge cannot", 13, 10
                db "listen to programs here.  (IOBPTEST shows the detail.)", 13, 10, 0
msg_spk_gate:   db ", sound rebuilt ", 0
msg_spk_dummy:  db 0
msg_spk_end:    db " times", 13, 10, 0
msg_spk_panic:  db "  Those ports were being flooded, so it stood down on its own.", 13, 10, 0
msg_spk_ports:  db "  writes 42h/43h/61h: ", 0
msg_spk_seen:   db ", traps ", 0
msg_spk_note:   db "  timer mode ", 0
msg_spk_div:    db ", divisor ", 0
msg_spk_gatenow: db ", gate ", 0
msg_spk_cost:   db "  worst cost to the machine ", 0
msg_spk_each:   db "%, about ", 0
msg_spk_cycles: db " cycles a trap", 13, 10, 0
msg_spk_drop:   db "  Port 61h was being polled hard, so it is no longer watched;", 13, 10
                db "  the gate is read directly instead.", 13, 10, 0
msg_spk_drop43: db "  Port 43h was being polled hard (the game reads the timer for", 13, 10
                db "  its own clock), so only the note itself is watched now.", 13, 10, 0
msg_spk_usage:  db "  SPEAKER ON    old games and BEEP become audible on laptops", 13, 10
                db "  SPEAKER OFF   leave the sound chip alone", 13, 10, 0
msg_copy_usage: db "Usage: COPY source destination", 13, 10, 0
msg_copy_read:  db "Cannot read the source file", 13, 10, 0
msg_copy_write: db "Cannot write the destination file", 13, 10, 0
msg_copy_done:  db "        1 file(s) copied, ", 0
msg_copy_bytes: db " bytes", 13, 10, 0
msg_del_usage:  db "Usage: DEL name", 13, 10, 0
msg_del_done:   db "        1 file(s) deleted", 13, 10, 0
msg_del_failed: db "Cannot delete that file", 13, 10, 0
msg_mkdir_usage: db "Usage: MKDIR name", 13, 10, 0
msg_mkdir_failed: db "Cannot create that directory", 13, 10, 0
msg_rmdir_usage: db "Usage: RMDIR name", 13, 10, 0
msg_rmdir_failed: db "Cannot remove it: a directory must exist, be empty and not be current", 13, 10, 0
msg_ren_usage:  db "Usage: REN oldname newname", 13, 10, 0
msg_ren_failed: db "Cannot rename that", 13, 10, 0
autoexec_name:  db "\AUTOEXEC.BAT", 0
after_pending:  db 0
batch_active:   db 0
batch_end:      db 0
batch_echo:     db 1
batch_pos:      dw 0
echo_dot:       db 0
args_ptr:       dw 0
dir_cluster:    dw 0
dir_files:      dw 0
dir_dirs:       dw 0
dir_bytes:      dd 0
tmp_cluster:    dw 0
type_remaining: dd 0
day_names:      db "Sun", 0, "Mon", 0, "Tue", 0, "Wed", 0, "Thu", 0, "Fri", 0, "Sat", 0

msg_bad_command:    db "Bad command or file name", 13, 10, 0
msg_no_memory:      db "Not enough memory to run the program", 13, 10, 0
msg_exec_error:     db "Cannot start the program (DOS error ", 0
msg_trace_on:       db "INT 21h tracing on (see EMBER.LOG)", 13, 10, 0
msg_trace_off:      db "INT 21h tracing off", 13, 10, 0
msg_bad_format:     db "Program file is not a valid executable", 13, 10, 0
msg_mem_free:       db " KB free for programs", 13, 10, 0
msg_batch_nested:   db "Nested batch files are not supported.", 13, 10, 0
msg_batch_large:    db "Batch file too large.", 13, 10, 0
msg_no_fs:          db "No valid FAT filesystem was found on the boot disk.", 13, 10, 0
msg_path_not_found: db "Path not found", 13, 10, 0
msg_file_not_found: db "File not found", 13, 10, 0
msg_invalid_dir:    db "Invalid directory", 13, 10, 0
msg_path_too_long:  db "Path too long", 13, 10, 0
msg_missing_param:  db "Required parameter missing", 13, 10, 0
msg_echo_on:        db "ECHO is on", 13, 10, 0
msg_echo_off:       db "ECHO is off", 13, 10, 0
msg_volume:         db " Volume in drive ", 0
msg_volume2:        db " is ", 0
msg_directory:      db " Directory of ", 0
msg_dir_tag:        db "    <DIR>   ", 0
msg_files:          db " file(s)", 0
msg_bytes:          db " bytes", 13, 10, 0
msg_dirs:           db " dir(s) ", 0
msg_bytes_free:     db " bytes free", 13, 10, 0
msg_time:           db "Current time is ", 0
msg_date:           db "Current date is ", 0
msg_mem_conv:       db " KB conventional memory", 13, 10, 0
msg_mem_ext:        db " KB extended memory (above 1 MB)", 13, 10, 0
msg_mem_kernel:     db " KB used by the Ember kernel at 0800:0000", 13, 10, 0
msg_color_usage:    db "Usage: COLOR <bf>   (two hex digits: background, foreground, e.g. COLOR 1F)", 13, 10, 0
msg_pause:          db "Press any key to continue . . . ", 0
msg_rebooting:      db "Rebooting...", 13, 10, 0
msg_shutdown:       db 13, 10, 10, "          Ember is shutting down...", 13, 10, 0
msg_safe_off:       db 13, 10, 10, "          It is now safe to turn off your computer.", 13, 10, 0
msg_version:        db 13, 10, "Ember Version ", VERSION, " (", BUILD_STAMP, ")", 13, 10, 0
msg_help:
        db 13, 10
        db "Ember commands:", 13, 10
        db "  DIR [path]      List files          CD [path]       Change directory", 13, 10
        db "  TYPE <file>     Show a text file    ECHO <text>     Print text", 13, 10
        db "  CLS             Clear the screen    COLOR <bf>      Set colours (e.g. 1F)", 13, 10
        db "  TIME / DATE     Show clock          MEM             Memory summary", 13, 10
        db "  VER             Version             BEEP            PC speaker", 13, 10
        db "  PAUSE           Wait for a key      REM             Comment (batch)", 13, 10
        db "  WIN             Graphical shell     REBOOT          Restart the PC", 13, 10
        db "  SHUTDOWN        Power off (APM)     HELP            This text", 13, 10
        db "  PLAY file.wav   Play a WAV file     PLAY C E G > C  Notes on the speaker", 13, 10
        db "  SOUND           Sound chip status   SOUND DEBUG     Trace the sound probe", 13, 10
        db "  <name>          Run a .COM program or .BAT batch file", 13, 10, 13, 10, 0

section .bss
copy_src:       resb 80
copy_dst:       resb 80
spl_seg:        resw 1                          ; the splash screen's state
spl_w:          resw 1
spl_h:          resw 1
spl_x:          resw 1
spl_y:          resw 1
spl_left:       resw 1
spl_start:      resd 1
spl_top:        resw 1
spl_xstep:      resd 1
spl_dy0:        resw 1
spl_dy1:        resw 1
spl_colour:     resb 1
spl_has_wav:    resb 1
tab_line:       resw 1                          ; Tab completion's state
tab_len:        resw 1
tab_cap:        resw 1
tab_word:       resw 1
tab_name:       resw 1
tab_prefix_len: resw 1
tab_count:      resw 1
tab_dir_cluster: resw 1
tab_dir:        resb 1
tab_prefix:     resb 14
tab_common:     resb 14
copy_in:        resw 1
copy_out:       resw 1
copy_total:     resd 1
copy_buf:       resb COPY_CHUNK
batch_buf:      resb BATCH_MAX + 1
after_buf:      resb AFTER_MAX
cmdline:        resb CMDLINE_MAX + 1
cmd_word:       resb 16
tmp_path:       resb 64
name_str:       resb 16
section .text
