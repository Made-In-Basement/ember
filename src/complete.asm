; =============================================================================
;  complete.asm - Tab completion at the prompt
; -----------------------------------------------------------------------------
;  The line editor calls [readline_tab] when Tab is pressed, if the shell
;  has set it.  The word under the cursor is matched against the names in
;  its directory: one match is filled in whole (a directory gets a "\"
;  after it), several are extended as far as they agree, and when nothing
;  more can be added the matches are listed and the line drawn again.
;
;  Entry: DS:DI = the line, BX = its length, DX = capacity.
;  Exit:  BX = the new length; whatever was added has been echoed.
; =============================================================================

shell_complete:
        pusha
        mov     [tab_line], di
        mov     [tab_len], bx
        mov     [tab_cap], dx
        ; ---- the word: back from the end to a space ----
        mov     bx, di                          ; BX = the line, for addressing
        mov     si, [tab_len]
.back:  test    si, si
        jz      .word_start
        cmp     byte [bx+si-1], ' '
        je      .word_start
        dec     si
        jmp     .back
.word_start:
        mov     [tab_word], si
        ; ---- split it at the last \ / or : ----
        mov     cx, si                          ; CX = where the name part starts
        mov     si, [tab_word]
.split: cmp     si, [tab_len]
        jae     .split_done
        mov     al, [bx+si]
        cmp     al, '\'
        je      .sep
        cmp     al, '/'
        je      .sep
        cmp     al, ':'
        jne     .split_next
.sep:   mov     cx, si
        inc     cx
.split_next:
        inc     si
        jmp     .split
.split_done:
        mov     [tab_name], cx
        ; the typed name part, upper-cased, into tab_prefix
        push    di
        mov     si, di
        add     si, cx
        mov     di, tab_prefix
        mov     cx, [tab_len]
        sub     cx, [tab_name]
        mov     [tab_prefix_len], cx
        cmp     cx, 12
        ja      .give_up_pop
        jcxz    .prefix_done
.up:    lodsb
        call    upcase
        stosb
        loop    .up
.prefix_done:
        mov     byte [di], 0
        pop     di
        ; ---- which directory ----
        mov     ax, [cur_dir_cluster]
        mov     cx, [tab_name]
        cmp     cx, [tab_word]
        je      .have_dir                       ; no directory part: here
        push    di
        mov     si, di
        add     si, [tab_word]
        mov     di, copy_dst
        sub     cx, [tab_word]                  ; CX = length of the directory part
        push    cx
        rep     movsb
        pop     cx
        mov     byte [di], 0
        ; "\" alone is the root; otherwise drop the separator and look it up
        cmp     cx, 1
        jne     .lookup
        cmp     byte [copy_dst], '\'
        je      .root_pop
        cmp     byte [copy_dst], '/'
        je      .root_pop
.lookup:
        cmp     byte [di-1], ':'                ; "C:" means where we are
        je      .here_pop
        mov     byte [di-1], 0
        mov     si, copy_dst
        call    resolve_path
        pop     di
        jc      .give_up
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .give_up
        mov     ax, [found_cluster]
        jmp     .have_dir
.root_pop:
        pop     di
        xor     ax, ax
        jmp     .have_dir
.here_pop:
        pop     di
        mov     ax, [cur_dir_cluster]
.have_dir:
        mov     [tab_dir_cluster], ax
        ; ---- walk it, keeping the first match and how far all matches agree ----
        mov     word [tab_count], 0
        mov     byte [tab_dir], 0
        call    dir_open
.entry: call    dir_next
        jc      .walked
        mov     al, [si]
        or      al, al
        jz      .walked
        cmp     al, 0xE5
        je      .entry
        mov     al, [si+11]
        cmp     al, ATTR_LFN
        je      .entry
        test    al, ATTR_VOLUME
        jnz     .entry
        cmp     byte [si], '.'
        jne     .real
        cmp     byte [tab_prefix], '.'          ; "." and ".." only if asked for
        jne     .entry
.real:  push    si
        mov     di, name_str
        call    fat_name_to_str                 ; DS:name_str = "NAME.EXT", CX = length
        pop     si
        ; does it start with the prefix?
        push    si
        mov     si, tab_prefix
        mov     di, name_str
        mov     cx, [tab_prefix_len]
        jcxz    .matches
        repe    cmpsb
        pop     si
        jne     .entry
        push    si
.matches:
        pop     si
        inc     word [tab_count]
        cmp     word [tab_count], 1
        jne     .agree
        ; the first match: remember it whole
        push    si
        mov     si, name_str
        mov     di, tab_common
        mov     cx, 13
        rep     movsb
        pop     si
        mov     al, [si+11]
        and     al, ATTR_DIRECTORY
        mov     [tab_dir], al
        jmp     .entry
.agree: ; shorten the common part to where this name still agrees
        push    si
        mov     si, name_str
        mov     di, tab_common
.cmp:   mov     al, [si]
        or      al, al
        jz      .cut
        cmp     al, [di]
        jne     .cut
        inc     si
        inc     di
        jmp     .cmp
.cut:   mov     byte [di], 0
        pop     si
        jmp     .entry
.walked:
        mov     di, [tab_line]
        mov     bx, [tab_len]
        cmp     word [tab_count], 0
        je      .give_up
        ; ---- how much can be added? ----
        mov     si, tab_common
        call    strlen_si                       ; CX = length of the common part
        cmp     cx, [tab_prefix_len]
        ja      .extend
        ; nothing to add: one match gets its "\" or a space, several get listed
        cmp     word [tab_count], 1
        je      .finish_one
        jmp     .list
.extend:
        ; replace the typed name part with the common part
        mov     bx, [tab_name]
        mov     si, tab_common
.put:   lodsb
        or      al, al
        jz      .put_done
        cmp     bx, [tab_cap]
        jae     .put_done
        mov     [di+bx], al
        inc     bx
        jmp     .put
.put_done:
        cmp     word [tab_count], 1
        jne     .redraw
.finish_one:
        ; a lone match is complete: a directory continues, a file is done
        mov     al, '\'
        cmp     byte [tab_dir], 0
        jne     .tail
        mov     al, ' '
.tail:  cmp     bx, [tab_cap]
        jae     .redraw
        mov     [di+bx], al
        inc     bx
        jmp     .redraw
.list:  ; ---- show every match, then the prompt and the line again ----
        call    crlf
        push    di
        push    bx
        mov     ax, [tab_dir_cluster]
        call    dir_open
.lentry:
        call    dir_next
        jc      .listed
        mov     al, [si]
        or      al, al
        jz      .listed
        cmp     al, 0xE5
        je      .lentry
        mov     al, [si+11]
        cmp     al, ATTR_LFN
        je      .lentry
        test    al, ATTR_VOLUME
        jnz     .lentry
        cmp     byte [si], '.'
        jne     .lreal
        cmp     byte [tab_prefix], '.'
        jne     .lentry
.lreal: push    si
        mov     di, name_str
        call    fat_name_to_str
        mov     si, tab_prefix
        mov     di, name_str
        mov     cx, [tab_prefix_len]
        jcxz    .lshow
        repe    cmpsb
        jne     .lskip
.lshow: mov     si, name_str
        call    puts
        mov     al, ' '
        call    putc
        mov     al, ' '
        call    putc
.lskip: pop     si
        jmp     .lentry
.listed:
        call    crlf
        call    print_prompt
        pop     bx
        pop     di
        ; the line as it stands
        mov     bp, di                          ; (SS = DS at the prompt)
        xor     si, si
.echo:  cmp     si, bx
        jae     .done
        mov     al, [bp+si]
        call    putc
        inc     si
        jmp     .echo
.redraw:
        ; take the typed name off the screen and show the word as it now is
        mov     cx, [tab_prefix_len]
        jcxz    .show
.erase: mov     al, 8
        call    putc
        mov     al, ' '
        call    putc
        mov     al, 8
        call    putc
        loop    .erase
.show:  mov     bp, di
        mov     si, [tab_name]
.added: cmp     si, bx
        jae     .done
        mov     al, [bp+si]
        call    putc
        inc     si
        jmp     .added
.done:  mov     [tab_len], bx
        popa
        mov     bx, [tab_len]
        ret
.give_up_pop:
        pop     di
.give_up:
        mov     al, 7                           ; a beep: nothing to offer
        call    putc
        popa
        ret

; strlen_si: CX = length of the NUL-terminated string at DS:SI
strlen_si:
        push    si
        xor     cx, cx
.n:     cmp     byte [si], 0
        je      .d
        inc     si
        inc     cx
        jmp     .n
.d:     pop     si
        ret
