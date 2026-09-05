; =============================================================================
;  apps.asm - applications of the graphical shell
;             File Manager ("My Computer"), Notepad (viewer), Calculator, About
; -----------------------------------------------------------------------------
;  Each app provides open / draw / click / key handlers (tables in gui.asm).
;  draw runs with cl_x/cl_y/cl_w/cl_h = client rectangle; click gets AX,BX
;  relative to the client area; key gets AL = ASCII, AH = scan code.
; =============================================================================

FM_MAX          equ 128                 ; directory entries kept
FM_LIST         equ 0x0000              ; GUI_SEG offset of the entry list
NP_TEXT         equ 0x1000              ; GUI_SEG offset of the text buffer
NP_MAX          equ 0xA000              ; 40 KB of text
NP_LINES        equ 0xC000              ; GUI_SEG offset of the line table
NP_MAX_LINES    equ 2000

; =============================================================================
; File Manager
; =============================================================================
fm_open:
        call    fm_load
        mov     word [fm_sel], 0
        mov     word [fm_top], 0
        ret

; fm_load: copy the current directory into GUI_SEG:FM_LIST and build the title
fm_load:
        pusha
        xor     di, di
        xor     cx, cx
        mov     ax, [cur_dir_cluster]
        call    dir_open
.next:  cmp     cx, FM_MAX
        jae     .done
        call    dir_next_visible
        jc      .done
        cmp     byte [si], '.'
        jne     .keep
        cmp     byte [si+1], ' '                ; skip "." (keep "..")
        je      .next
.keep:  push    cx
        mov     cx, 32
.copy:  lodsb
        mov     [fs:di], al
        inc     di
        loop    .copy
        pop     cx
        inc     cx
        jmp     .next
.done:  mov     [fm_count], cx
        mov     si, str_my_computer
        mov     di, fm_title
        call    copy_str
        mov     si, str_sep
        call    copy_str
        mov     al, [drive_letter]
        stosb
        mov     al, ':'
        stosb
        mov     si, cur_path
        call    copy_str
        xor     al, al
        stosb
        popa
        ret

; copy_str: copy DS:SI to DS:DI without the terminator; DI advanced
copy_str:
        push    ax
        push    si
.next:  lodsb
        or      al, al
        jz      .done
        stosb
        jmp     .next
.done:  pop     si
        pop     ax
        ret

; fm_entry: AX = index -> SI = GUI_SEG offset of the 32-byte entry
fm_entry:
        mov     si, ax
        shl     si, 5
        add     si, FM_LIST
        ret

; fm_fetch_name: AX = index -> name_str2 = "NAME.EXT", fm_attr = attributes
fm_fetch_name:
        pusha
        call    fm_entry
        mov     di, fm_name11
        mov     cx, 11
.copy:  mov     al, [fs:si]
        mov     [di], al
        inc     si
        inc     di
        loop    .copy
        mov     al, [fs:si]
        mov     [fm_attr], al
        mov     si, fm_name11
        mov     di, name_str2
        call    fat_name_to_str
        popa
        ret

fm_draw:
        pusha
        mov     byte [pen], C_WHITE
        mov     ax, [cl_x]
        mov     bx, [cl_y]
        mov     cx, [cl_w]
        mov     dx, [cl_h]
        call    gfx_fill_rect
        movzx   ax, byte [font_h]
        add     ax, 2
        mov     [fm_row_h], ax
        mov     ax, [cl_h]
        sub     ax, 4
        xor     dx, dx
        div     word [fm_row_h]
        mov     [fm_rows], ax
        xor     si, si                          ; SI = row
.row:   cmp     si, [fm_rows]
        jae     .done
        mov     di, [fm_top]
        add     di, si                          ; DI = entry index
        cmp     di, [fm_count]
        jae     .done
        mov     ax, si
        mul     word [fm_row_h]
        add     ax, [cl_y]
        add     ax, 2
        mov     bx, ax                          ; BX = row y
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_WHITE
        cmp     di, [fm_sel]
        jne     .no_hl
        mov     byte [pen], C_NAVY
        mov     ax, [cl_x]
        add     ax, 2
        mov     cx, [cl_w]
        sub     cx, 4
        mov     dx, [fm_row_h]
        call    gfx_fill_rect
        mov     byte [text_fg], C_WHITE
        mov     byte [text_bg], C_NAVY
.no_hl: mov     ax, di
        call    fm_fetch_name                   ; name_str2, fm_attr
        ; icon
        push    si
        mov     si, small_file
        test    byte [fm_attr], ATTR_DIRECTORY
        jz      .not_dir
        mov     si, small_folder
        jmp     .icon
.not_dir:
        call    fm_is_program
        jnc     .icon
        mov     si, small_prog
.icon:  mov     ax, [cl_x]
        add     ax, 6
        push    bx
        mov     cx, [fm_row_h]
        sub     cx, SMALL_H
        shr     cx, 1
        add     bx, cx
        mov     cx, SMALL_W
        mov     dx, SMALL_H
        call    gfx_draw_image
        pop     bx
        pop     si
        ; name
        mov     ax, [cl_x]
        add     ax, 28
        push    bx
        inc     bx
        push    si
        mov     si, name_str2
        call    gfx_text
        pop     si
        ; size or <DIR>
        push    si
        mov     si, str_dir_tag
        test    byte [fm_attr], ATTR_DIRECTORY
        jnz     .size_str
        mov     ax, di
        call    fm_entry
        mov     eax, [fs:si+28]
        mov     di, num_str
        call    fmt_dec
        mov     si, num_str
.size_str:
        call    text_len                        ; DX = length
        shl     dx, 3
        mov     ax, [cl_x]
        add     ax, [cl_w]
        sub     ax, 10
        sub     ax, dx
        call    gfx_text
        pop     si
        pop     bx
        inc     si
        jmp     .row
.done:  popa
        ret

; fm_is_program: CF=1 if fm_name11 has extension COM or BAT
fm_is_program:
        cmp     word [fm_name11+8], "CO"
        jne     .bat
        cmp     byte [fm_name11+10], 'M'
        jne     .no
        stc
        ret
.bat:   cmp     word [fm_name11+8], "BA"
        jne     .no
        cmp     byte [fm_name11+10], 'T'
        jne     .no
        stc
        ret
.no:    clc
        ret

fm_click:
        pusha
        mov     ax, bx
        sub     ax, 2
        js      .done
        xor     dx, dx
        div     word [fm_row_h]
        add     ax, [fm_top]
        cmp     ax, [fm_count]
        jae     .done
        mov     [fm_sel], ax
        add     ax, 100
        call    check_double_click
        jc      .activate
        call    gui_redraw_top
        jmp     .done
.activate:
        call    fm_activate
.done:  popa
        ret

fm_key:
        pusha
        cmp     al, 13
        je      .enter
        cmp     al, 8
        je      .parent
        cmp     ah, 0x48
        je      .up
        cmp     ah, 0x50
        je      .down
        cmp     ah, 0x49
        je      .pgup
        cmp     ah, 0x51
        je      .pgdn
        cmp     ah, 0x47
        je      .home
        cmp     ah, 0x4F
        je      .end
        jmp     .done
.up:    mov     ax, -1
        jmp     .move
.down:  mov     ax, 1
        jmp     .move
.pgup:  mov     ax, [fm_rows]
        neg     ax
        jmp     .move
.pgdn:  mov     ax, [fm_rows]
        jmp     .move
.home:  mov     ax, -32000
        jmp     .move
.end:   mov     ax, 32000
.move:  add     ax, [fm_sel]
        jns     .clamp_hi
        xor     ax, ax
.clamp_hi:
        mov     cx, [fm_count]
        dec     cx
        js      .done
        cmp     ax, cx
        jle     .set
        mov     ax, cx
.set:   mov     [fm_sel], ax
        ; keep the selection visible
        cmp     ax, [fm_top]
        jae     .below_top
        mov     [fm_top], ax
.below_top:
        mov     cx, [fm_top]
        add     cx, [fm_rows]
        cmp     ax, cx
        jb      .redraw
        sub     ax, [fm_rows]
        inc     ax
        mov     [fm_top], ax
.redraw:
        call    gui_redraw_top
        jmp     .done
.enter: call    fm_activate
        jmp     .done
.parent:
        cmp     word [cur_dir_cluster], 0
        je      .done
        mov     si, str_dotdot
        call    fm_change_dir
.done:  popa
        ret

; fm_change_dir: DS:SI = directory name ("..", "DOCS") -> CD and reload
fm_change_dir:
        pusha
        call    cmd_cd
        call    fm_load
        mov     word [fm_sel], 0
        mov     word [fm_top], 0
        call    gui_redraw
        popa
        ret

; fm_activate: open the selected entry
fm_activate:
        pusha
        mov     ax, [fm_sel]
        cmp     ax, [fm_count]
        jae     .done
        call    fm_fetch_name                   ; name_str2, fm_attr, fm_name11
        test    byte [fm_attr], ATTR_DIRECTORY
        jz      .file
        mov     si, name_str2
        call    fm_change_dir
        jmp     .done
.file:  call    fm_is_program
        jnc     .text
        mov     si, name_str2
        call    gui_run_program
        jmp     .done
.text:  mov     si, fm_name11
        mov     ax, [cur_dir_cluster]
        call    find_entry
        jc      .done
        mov     si, fm_name11
        mov     di, fat_name                    ; np_load titles from fat_name
        mov     cx, 11
        rep     movsb
        call    np_load
        mov     ax, WIN_NOTEPAD
        call    win_open
.done:  popa
        ret

; -----------------------------------------------------------------------------
; gui_run_program: leave graphics mode, run DS:SI (NAME.EXT), come back
; -----------------------------------------------------------------------------
gui_run_program:
        pusha
        push    si
        call    cursor_hide
        call    mouse_stop
        call    gfx_done
        mov     byte [screen_attr], 0x07
        call    cls
        mov     si, msg_running
        call    puts
        pop     si
        push    si
        call    puts
        call    crlf
        call    crlf
        pop     si
        push    si
        call    resolve_path
        pop     si
        jc      .back
        cmp     word [fat_name+8], "BA"
        je      .batch
        mov     di, str_empty
        call    run_program_file
        jmp     .back
.batch: call    run_batch
.back:  mov     fs, [gui_seg]
        mov     si, msg_return
        call    puts
        call    kb_flush
        call    getkey
        mov     al, [gui_vmode]
        call    gfx_init
        cmp     byte [gui_nomouse], 0
        jne     .no_mouse
        call    mouse_init
.no_mouse:
        mov     byte [cursor_visible], 0
        mov     byte [clock_minute], 0xFF
        call    gui_redraw
        popa
        ret

; =============================================================================
; Notepad (read-only viewer)
; =============================================================================
np_open:
        pusha
        cmp     byte [np_loaded], 0
        jne     .done
        mov     si, str_readme_path
        call    resolve_path
        jc      .done
        call    np_load
.done:  mov     word [np_top], 0
        popa
        ret

; np_load: load the file described by found_* / fat_name into GUI_SEG
np_load:
        pusha
        push    es
        mov     eax, [found_size]
        cmp     eax, NP_MAX
        jbe     .size_ok
        mov     eax, NP_MAX
.size_ok:
        mov     [np_size], ax
        mov     es, [gui_seg]
        mov     bx, NP_TEXT
        mov     ax, [found_cluster]
        mov     cx, NP_MAX / 512
        call    load_chain
        pop     es
        ; line table
        xor     si, si
        mov     word [fs:NP_LINES], 0
        mov     di, 1
.scan:  cmp     si, [np_size]
        jae     .scanned
        mov     al, [fs:NP_TEXT+si]
        inc     si
        cmp     al, 10
        jne     .scan
        cmp     di, NP_MAX_LINES
        jae     .scanned
        mov     bx, di
        shl     bx, 1
        mov     [fs:NP_LINES+bx], si
        inc     di
        jmp     .scan
.scanned:
        mov     [np_lines], di
        ; title
        mov     si, str_notepad
        mov     di, np_title
        call    copy_str
        mov     si, str_sep
        call    copy_str
        mov     si, fat_name
        call    fat_name_to_str                 ; writes NAME.EXT + NUL at DI
        mov     byte [np_loaded], 1
        mov     word [np_top], 0
        popa
        ret

np_draw:
        pusha
        mov     byte [pen], C_WHITE
        mov     ax, [cl_x]
        mov     bx, [cl_y]
        mov     cx, [cl_w]
        mov     dx, [cl_h]
        call    gfx_fill_rect
        mov     ax, [cl_h]
        sub     ax, 4
        xor     dx, dx
        movzx   cx, byte [font_h]
        div     cx
        mov     [np_rows], ax
        mov     ax, [cl_w]
        sub     ax, 24
        shr     ax, 3
        cmp     ax, 120
        jbe     .cols_ok
        mov     ax, 120
.cols_ok:
        mov     [np_cols], ax
        cmp     byte [np_loaded], 0
        jne     .text
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_WHITE
        mov     ax, [cl_x]
        add     ax, 8
        mov     bx, [cl_y]
        add     bx, 8
        mov     si, msg_no_file
        call    gfx_text
        jmp     .done
.text:  mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_WHITE
        xor     dx, dx                          ; DX = row
.row:   cmp     dx, [np_rows]
        jae     .rows_done
        mov     ax, [np_top]
        add     ax, dx
        cmp     ax, [np_lines]
        jae     .rows_done
        call    np_build_row                    ; AX = line -> row_buf (np_cols chars)
        push    dx
        movzx   ax, byte [font_h]
        mul     dx
        add     ax, [cl_y]
        add     ax, 2
        mov     bx, ax
        mov     ax, [cl_x]
        add     ax, 4
        mov     si, row_buf
        mov     cx, [np_cols]
        call    gfx_text_n
        pop     dx
        inc     dx
        jmp     .row
.rows_done:
        call    np_draw_scrollbar
.done:  popa
        ret

; np_build_row: AX = line number -> row_buf filled with np_cols characters
np_build_row:
        pusha
        mov     si, ax
        shl     si, 1
        mov     si, [fs:NP_LINES+si]            ; SI = text offset
        mov     di, row_buf
        mov     cx, [np_cols]
        xor     dl, dl                          ; DL = 1 once the line ended
.next:  mov     al, ' '
        or      dl, dl
        jnz     .store
        cmp     si, [np_size]
        jae     .ended
        mov     al, [fs:NP_TEXT+si]
        inc     si
        cmp     al, 13
        je      .ended
        cmp     al, 10
        je      .ended
        cmp     al, 9
        jne     .store
        mov     al, ' '
        jmp     .store
.ended: mov     dl, 1
        mov     al, ' '
.store: mov     [di], al
        inc     di
        loop    .next
        popa
        ret

np_draw_scrollbar:
        pusha
        mov     ax, [np_lines]
        cmp     ax, [np_rows]
        jbe     .done
        mov     ax, [cl_x]
        add     ax, [cl_w]
        sub     ax, 16
        mov     [np_sb_x], ax
        mov     bx, [cl_y]
        mov     cx, 16
        mov     dx, [cl_h]
        mov     byte [pen], C_LTGRAY
        call    gfx_fill_rect
        ; up button
        mov     dx, 16
        mov     byte [bevel], 0
        call    gfx_bevel
        push    ax
        push    bx
        add     ax, 8
        add     bx, 5
        mov     byte [pen], C_BLACK
        mov     cx, 1
        call    gfx_hline
        dec     ax
        inc     bx
        mov     cx, 3
        call    gfx_hline
        dec     ax
        inc     bx
        mov     cx, 5
        call    gfx_hline
        pop     bx
        pop     ax
        ; down button
        push    bx
        add     bx, [cl_h]
        sub     bx, 16
        mov     cx, 16
        call    gfx_bevel
        push    ax
        push    bx
        add     ax, 6
        add     bx, 6
        mov     byte [pen], C_BLACK
        mov     cx, 5
        call    gfx_hline
        inc     ax
        inc     bx
        mov     cx, 3
        call    gfx_hline
        inc     ax
        inc     bx
        mov     cx, 1
        call    gfx_hline
        pop     bx
        pop     ax
        pop     bx
        ; thumb
        call    np_thumb_pos                    ; CX = thumb offset from the track top
        add     bx, 16
        add     bx, cx
        mov     [np_thumb_y], bx
        mov     cx, 16
        mov     dx, 16
        call    gfx_bevel
.done:  popa
        ret

; np_thumb_pos: CX = thumb offset within the track (0..track_h-16)
np_thumb_pos:
        push    ax
        push    bx
        push    dx
        mov     ax, [cl_h]
        sub     ax, 48                          ; track minus buttons and thumb
        mul     word [np_top]
        mov     bx, [np_lines]
        sub     bx, [np_rows]
        jz      .zero
        div     bx
        mov     cx, ax
        jmp     .done
.zero:  xor     cx, cx
.done:  pop     dx
        pop     bx
        pop     ax
        ret

; np_scroll: AX = signed line delta
np_scroll:
        push    ax
        push    cx
        add     ax, [np_top]
        jns     .lo_ok
        xor     ax, ax
.lo_ok: mov     cx, [np_lines]
        sub     cx, [np_rows]
        jns     .hi
        xor     cx, cx
.hi:    cmp     ax, cx
        jle     .set
        mov     ax, cx
.set:   mov     [np_top], ax
        pop     cx
        pop     ax
        ret

np_click:
        pusha
        mov     cx, [cl_w]
        sub     cx, 16
        cmp     ax, cx
        jb      .done                           ; only the scrollbar reacts
        mov     ax, [np_lines]
        cmp     ax, [np_rows]
        jbe     .done
        cmp     bx, 16
        jb      .up
        mov     cx, [cl_h]
        sub     cx, 16
        cmp     bx, cx
        jae     .down
        add     bx, [cl_y]
        cmp     bx, [np_thumb_y]
        jb      .page_up
        mov     ax, [np_rows]
        jmp     .scroll
.page_up:
        mov     ax, [np_rows]
        neg     ax
        jmp     .scroll
.up:    mov     ax, -1
        jmp     .scroll
.down:  mov     ax, 1
.scroll:
        call    np_scroll
        call    gui_redraw_top
.done:  popa
        ret

np_key:
        pusha
        cmp     ah, 0x48
        je      .up
        cmp     ah, 0x50
        je      .down
        cmp     ah, 0x49
        je      .pgup
        cmp     ah, 0x51
        je      .pgdn
        cmp     ah, 0x47
        je      .home
        cmp     ah, 0x4F
        je      .end
        jmp     .done
.up:    mov     ax, -1
        jmp     .scroll
.down:  mov     ax, 1
        jmp     .scroll
.pgup:  mov     ax, [np_rows]
        neg     ax
        jmp     .scroll
.pgdn:  mov     ax, [np_rows]
        jmp     .scroll
.home:  mov     ax, -32000
        jmp     .scroll
.end:   mov     ax, 32000
.scroll:
        call    np_scroll
        call    gui_redraw_top
.done:  popa
        ret

; =============================================================================
; Calculator
; =============================================================================
calc_open:
        mov     dword [calc_acc], 0
        mov     dword [calc_cur], 0
        mov     byte [calc_op], 0
        mov     byte [calc_new], 1
        mov     byte [calc_err], 0
        ret

calc_draw:
        pusha
        mov     byte [pen], C_SILVER
        mov     ax, [cl_x]
        mov     bx, [cl_y]
        mov     cx, [cl_w]
        mov     dx, [cl_h]
        call    gfx_fill_rect
        ; display
        mov     ax, [cl_x]
        add     ax, 6
        mov     bx, [cl_y]
        add     bx, 6
        mov     cx, [cl_w]
        sub     cx, 12
        movzx   dx, byte [font_h]
        add     dx, 8
        mov     byte [bevel], 1
        call    gfx_bevel
        add     ax, 2
        add     bx, 2
        sub     cx, 4
        sub     dx, 4
        mov     byte [pen], C_WHITE
        call    gfx_fill_rect
        mov     si, str_error
        cmp     byte [calc_err], 0
        jne     .show
        mov     eax, [calc_cur]
        mov     di, num_str
        call    fmt_dec
        mov     si, num_str
.show:  call    text_len                        ; DX = length
        shl     dx, 3
        mov     ax, [cl_x]
        add     ax, [cl_w]
        sub     ax, 10
        sub     ax, dx
        mov     bx, [cl_y]
        add     bx, 10
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_WHITE
        call    gfx_text
        ; button grid geometry
        mov     ax, [cl_x]
        add     ax, 6
        mov     [calc_gx], ax
        mov     ax, [cl_y]
        add     ax, 12
        movzx   cx, byte [font_h]
        add     ax, cx
        add     ax, 8
        mov     [calc_gy], ax
        mov     ax, [cl_w]
        sub     ax, 24
        shr     ax, 2
        mov     [calc_bw], ax
        mov     ax, [cl_y]
        add     ax, [cl_h]
        sub     ax, [calc_gy]
        sub     ax, 18
        shr     ax, 2
        mov     [calc_bh], ax
        xor     si, si
.btn:   mov     ax, si
        and     ax, 3
        mov     cx, [calc_bw]
        add     cx, 4
        mul     cx
        add     ax, [calc_gx]
        push    ax
        mov     ax, si
        shr     ax, 2
        mov     cx, [calc_bh]
        add     cx, 4
        mul     cx
        add     ax, [calc_gy]
        mov     bx, ax
        pop     ax
        mov     cx, [calc_bw]
        mov     dx, [calc_bh]
        mov     byte [bevel], 0
        call    gfx_bevel
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_SILVER
        push    si
        shl     si, 1
        add     si, calc_labels
        push    dx
        movzx   dx, byte [font_h]
        sub     dx, cx
        neg     dx
        ; centre the single character
        mov     dx, cx
        shr     dx, 1
        sub     dx, 4
        add     ax, dx
        pop     dx
        push    bx
        movzx   cx, byte [font_h]
        sub     dx, cx
        shr     dx, 1
        add     bx, dx
        call    gfx_text
        pop     bx
        pop     si
        inc     si
        cmp     si, 16
        jb      .btn
        popa
        ret

calc_labels:
        db "7", 0, "8", 0, "9", 0, "/", 0
        db "4", 0, "5", 0, "6", 0, "*", 0
        db "1", 0, "2", 0, "3", 0, "-", 0
        db "C", 0, "0", 0, "=", 0, "+", 0

calc_click:
        pusha
        add     ax, [cl_x]
        add     bx, [cl_y]
        sub     ax, [calc_gx]
        js      .done
        sub     bx, [calc_gy]
        js      .done
        mov     cx, [calc_bw]
        add     cx, 4
        xor     dx, dx
        div     cx                              ; AX = column, DX = remainder
        cmp     dx, [calc_bw]
        jae     .done
        cmp     ax, 4
        jae     .done
        mov     si, ax
        mov     ax, bx
        mov     cx, [calc_bh]
        add     cx, 4
        xor     dx, dx
        div     cx
        cmp     dx, [calc_bh]
        jae     .done
        cmp     ax, 4
        jae     .done
        shl     ax, 2
        add     si, ax
        shl     si, 1
        mov     al, [calc_labels+si]
        call    calc_input
        call    gui_redraw_top
.done:  popa
        ret

calc_key:
        pusha
        cmp     al, 13
        jne     .not_enter
        mov     al, '='
.not_enter:
        cmp     al, 'c'
        jne     .not_c
        mov     al, 'C'
.not_c: cmp     al, 8
        jne     .not_bs
        mov     al, 'B'
.not_bs:
        cmp     al, '0'
        jb      .op
        cmp     al, '9'
        jbe     .ok
.op:    cmp     al, '+'
        je      .ok
        cmp     al, '-'
        je      .ok
        cmp     al, '*'
        je      .ok
        cmp     al, '/'
        je      .ok
        cmp     al, '='
        je      .ok
        cmp     al, 'C'
        je      .ok
        cmp     al, 'B'
        jne     .done
.ok:    call    calc_input
        call    gui_redraw_top
.done:  popa
        ret

; calc_input: AL = key ('0'-'9', '+', '-', '*', '/', '=', 'C', 'B')
calc_input:
        pushad
        cmp     al, 'C'
        je      .clear
        cmp     byte [calc_err], 0
        jne     .done                           ; only C recovers from an error
        cmp     al, 'B'
        je      .backspace
        cmp     al, '0'
        jb      .operator
        cmp     al, '9'
        ja      .operator
        ; ---- digit ----
        cmp     byte [calc_new], 0
        je      .append
        mov     dword [calc_cur], 0
        mov     byte [calc_new], 0
.append:
        mov     ebx, [calc_cur]
        cmp     ebx, 99999999
        jg      .done
        cmp     ebx, -99999999
        jl      .done
        imul    ebx, 10
        movzx   eax, al
        sub     eax, '0'
        add     ebx, eax
        mov     [calc_cur], ebx
        jmp     .done
.backspace:
        cmp     byte [calc_new], 0
        jne     .done
        mov     eax, [calc_cur]
        cdq
        mov     ebx, 10
        idiv    ebx
        mov     [calc_cur], eax
        jmp     .done
.clear: mov     dword [calc_acc], 0
        mov     dword [calc_cur], 0
        mov     byte [calc_op], 0
        mov     byte [calc_new], 1
        mov     byte [calc_err], 0
        jmp     .done
.operator:
        cmp     al, '='
        je      .equals
        ; + - * /
        cmp     byte [calc_op], 0
        je      .take
        cmp     byte [calc_new], 0
        jne     .replace                        ; operator pressed twice
        call    calc_apply
        jmp     .replace
.take:  mov     ebx, [calc_cur]
        mov     [calc_acc], ebx
.replace:
        mov     [calc_op], al
        mov     ebx, [calc_acc]
        mov     [calc_cur], ebx
        mov     byte [calc_new], 1
        jmp     .done
.equals:
        cmp     byte [calc_op], 0
        je      .done
        call    calc_apply
        mov     byte [calc_op], 0
        mov     ebx, [calc_acc]
        mov     [calc_cur], ebx
        mov     byte [calc_new], 1
.done:  popad
        ret

; calc_apply: acc = acc <op> cur
calc_apply:
        push    eax
        push    ebx
        push    edx
        mov     eax, [calc_acc]
        mov     ebx, [calc_cur]
        mov     dl, [calc_op]
        cmp     dl, '+'
        jne     .sub
        add     eax, ebx
        jmp     .store
.sub:   cmp     dl, '-'
        jne     .mul
        sub     eax, ebx
        jmp     .store
.mul:   cmp     dl, '*'
        jne     .div
        imul    eax, ebx
        jmp     .store
.div:   test    ebx, ebx
        jz      .error
        cdq
        idiv    ebx
.store: mov     [calc_acc], eax
        jmp     .done
.error: mov     byte [calc_err], 1
        mov     dword [calc_acc], 0
.done:  pop     edx
        pop     ebx
        pop     eax
        ret

; =============================================================================
; About
; =============================================================================
about_open:
        ret

about_draw:
        pusha
        mov     byte [pen], C_SILVER
        mov     ax, [cl_x]
        mov     bx, [cl_y]
        mov     cx, [cl_w]
        mov     dx, [cl_h]
        call    gfx_fill_rect
        mov     ax, [cl_x]
        add     ax, 10
        mov     bx, [cl_y]
        add     bx, 10
        mov     cx, ICON_W
        mov     dx, ICON_H
        mov     si, icon_about
        call    gfx_draw_image
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_SILVER
        mov     di, [cl_x]
        add     di, 52
        mov     bx, [cl_y]
        add     bx, 10
        mov     si, about_lines
.line:  cmp     byte [si], 0xFF
        je      .lines_done
        mov     ax, di
        call    gfx_text
        movzx   cx, byte [font_h]
        add     cx, 2
        add     bx, cx
        call    text_len
        add     si, dx
        inc     si
        jmp     .line
.lines_done:
        mov     ax, di
        mov     si, str_mouse_on
        cmp     byte [mouse_bios], 0
        je      .direct
        mov     si, str_mouse_bios
.direct:
        cmp     byte [mouse_ok], 0
        jne     .mouse_line
        mov     si, str_mouse_off
        mov     cl, [mouse_fail_step]
        add     cl, '0'
        mov     [str_mouse_off+18], cl
.mouse_line:
        call    gfx_text
        ; OK button
        mov     ax, [cl_w]
        sub     ax, 80
        shr     ax, 1
        add     ax, [cl_x]
        mov     [about_btn_x], ax
        mov     bx, [cl_y]
        add     bx, [cl_h]
        sub     bx, [L_btn_h]
        sub     bx, 8
        mov     [about_btn_y], bx
        mov     cx, 80
        mov     dx, [L_btn_h]
        mov     byte [bevel], 0
        call    gfx_bevel
        mov     si, str_ok
        push    bx
        movzx   cx, byte [font_h]
        sub     dx, cx
        shr     dx, 1
        add     bx, dx
        mov     cx, 80
        call    gfx_text_center
        pop     bx
        popa
        ret

about_lines:
        db "Ember Windows 1.0", 0
        db "A graphical shell for", 0
        db "Ember, written in", 0
        db "x86 assembly language.", 0
        db "", 0
        db "Esc closes  Tab switches", 0
        db "Ctrl+Esc opens Start", 0
        db 0xFF
str_mouse_on:   db "Mouse: PS/2 driver active", 0
str_mouse_bios: db "Mouse: active (BIOS-assisted)", 0
str_mouse_off:  db "Mouse: none (code 0)", 0

about_click:
        pusha
        add     ax, [cl_x]
        add     bx, [cl_y]
        cmp     ax, [about_btn_x]
        jb      .done
        mov     cx, [about_btn_x]
        add     cx, 80
        cmp     ax, cx
        jae     .done
        cmp     bx, [about_btn_y]
        jb      .done
        mov     cx, [about_btn_y]
        add     cx, [L_btn_h]
        cmp     bx, cx
        jae     .done
        mov     ax, WIN_ABOUT
        call    win_close
.done:  popa
        ret

about_key:
        cmp     al, 13
        jne     .done
        push    ax
        mov     ax, WIN_ABOUT
        call    win_close
        pop     ax
.done:  ret

; =============================================================================
section .data
fm_count:       dw 0
fm_sel:         dw 0
fm_top:         dw 0
fm_rows:        dw 1
fm_row_h:       dw 18
fm_attr:        db 0
np_loaded:      db 0
np_size:        dw 0
np_lines:       dw 0
np_top:         dw 0
np_rows:        dw 1
np_cols:        dw 1
np_sb_x:        dw 0
np_thumb_y:     dw 0
calc_acc:       dd 0
calc_cur:       dd 0
calc_op:        db 0
calc_new:       db 1
calc_err:       db 0
calc_gx:        dw 0
calc_gy:        dw 0
calc_bw:        dw 0
calc_bh:        dw 0
about_btn_x:    dw 0
about_btn_y:    dw 0
str_sep:        db " - ", 0
str_dir_tag:    db "<DIR>", 0
str_dotdot:     db "..", 0
str_empty:      db 0
str_ok:         db "OK", 0
str_error:      db "Error", 0
str_readme_path: db "\README.TXT", 0
msg_no_file:    db "No file loaded. Open a text file from My Computer.", 0
msg_running:    db "Running ", 0
msg_return:     db 13, 10, "Press any key to return to the graphical shell . . . ", 0
section .bss
fm_title:       resb 96
np_title:       resb 32
fm_name11:      resb 12
name_str2:      resb 16
num_str:        resb 16
row_buf:        resb 128
section .text
