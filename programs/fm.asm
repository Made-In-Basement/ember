; =============================================================================
;  FM.COM - a two-panel file manager for Ember
; -----------------------------------------------------------------------------
;  Two directory panels side by side, the way it has been done since 1986:
;  Tab switches panels, Enter opens a directory or runs a program, and the
;  function keys do the work:
;
;    F1 help   F3 view   F5 copy   F6 rename / move   F7 make directory
;    F8 delete   F10 quit   Backspace goes up a directory   Ctrl+R rereads
;
;  Enter on a .COM / .EXE / .N32 runs it and comes back; on an .MP3 or .WAV
;  it starts the music player; on a .BAT it runs the lines itself (CD,
;  ECHO, REM, PAUSE and programs); on anything else it opens the viewer.
;  Plain DOS calls throughout (INT 21h); the screen is written directly.
; =============================================================================

[BITS 16]
[ORG 0x0100]

VIDEO           equ 0xB800
ROWS            equ 20                          ; entries visible in a panel
MAX_ENTRIES     equ 256
LONG_MAX        equ 32                          ; of a long name, what we keep
ENTRY_SIZE      equ 52                          ; name[13] attr size[4] long[32]
E_NAME          equ 0
E_ATTR          equ 13
E_SIZE          equ 14
E_LONG          equ 20
P_PATH          equ 0                           ; a panel: path[64] count top cur
P_COUNT         equ 64
P_TOP           equ 66
P_CUR           equ 68
PANEL_SIZE      equ 72
IOBUF_SIZE      equ 16384
MAX_LINES       equ 1024

A_FRAME         equ 0x1B                        ; cyan on blue
A_FILE          equ 0x1B
A_DIR           equ 0x1F                        ; white on blue
A_CUR           equ 0x30                        ; black on cyan
A_PATH          equ 0x30
A_KEYNUM        equ 0x07
A_KEY           equ 0x30
A_MSG           equ 0x0F
A_INPUT         equ 0x70
A_VIEW          equ 0x1B
A_HELP          equ 0x70

start:
        cld
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        ; DOS hands a .COM program every free byte.  Keep our own 64 KB
        ; segment (the stack lives at the top of it) and give back the rest,
        ; or there is no room to run anything from here.
        mov     bx, 0x1000
        mov     ah, 0x4A
        int     0x21
        mov     ax, VIDEO
        mov     fs, ax
        mov     dx, dta
        mov     ah, 0x1A                        ; our transfer area
        int     0x21
        ; both panels start where we are
        mov     byte [panel_l+P_PATH], '\'
        mov     si, panel_l + P_PATH + 1
        xor     dl, dl
        mov     ah, 0x47
        int     0x21
        mov     si, panel_l + P_PATH
        mov     di, panel_r + P_PATH
        mov     cx, 64
        rep     movsb
        mov     byte [active], 0
        call    read_both
        call    draw_all

main_loop:
        call    get_key
        cmp     al, 9
        je      .switch
        cmp     al, 13
        je      .enter
        cmp     al, 8
        je      .parent
        cmp     al, 27
        je      quit
        cmp     al, 18                          ; Ctrl+R
        je      .reread
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
        cmp     ah, 0x3B                        ; F1
        je      .help
        cmp     ah, 0x3D                        ; F3
        je      .view
        cmp     ah, 0x3F                        ; F5
        je      .copy
        cmp     ah, 0x40                        ; F6
        je      .move
        cmp     ah, 0x41                        ; F7
        je      .mkdir
        cmp     ah, 0x42                        ; F8
        je      .delete
        cmp     ah, 0x44                        ; F10
        je      quit
        jmp     main_loop
.switch:
        xor     byte [active], 1
        call    draw_all
        jmp     main_loop
.up:    call    cur_panel
        cmp     word [bx+P_CUR], 0
        je      main_loop
        dec     word [bx+P_CUR]
        jmp     .moved
.down:  call    cur_panel
        mov     ax, [bx+P_CUR]
        inc     ax
        cmp     ax, [bx+P_COUNT]
        jae     main_loop
        mov     [bx+P_CUR], ax
        jmp     .moved
.pgup:  call    cur_panel
        mov     ax, [bx+P_CUR]
        sub     ax, ROWS
        jns     .set
        xor     ax, ax
        jmp     .set
.pgdn:  call    cur_panel
        mov     ax, [bx+P_CUR]
        add     ax, ROWS
        cmp     ax, [bx+P_COUNT]
        jb      .set
.end:   call    cur_panel
        mov     ax, [bx+P_COUNT]
        dec     ax
        jns     .set
        xor     ax, ax
        jmp     .set
.home:  call    cur_panel
        xor     ax, ax
.set:   mov     [bx+P_CUR], ax
.moved: call    draw_active
        jmp     main_loop
.reread:
        call    read_both
        call    draw_all
        jmp     main_loop
.parent:
        call    go_parent
        jmp     main_loop
.enter: call    do_enter
        jmp     main_loop
.help:  call    show_help
        jmp     main_loop
.view:  call    do_view
        jmp     main_loop
.copy:  call    do_copy
        jmp     main_loop
.move:  call    do_move
        jmp     main_loop
.mkdir: call    do_mkdir
        jmp     main_loop
.delete:
        call    do_delete
        jmp     main_loop

quit:   ; a clean screen, standing in the active panel's directory
        call    cur_panel
        lea     dx, [bx+P_PATH]
        mov     ah, 0x3B
        int     0x21
        mov     ax, 0x0003
        int     0x10
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------- panels
; cur_panel: BX -> the active panel, SI -> its entries
cur_panel:
        mov     bx, panel_l
        mov     si, entries_l
        cmp     byte [active], 0
        je      .done
        mov     bx, panel_r
        mov     si, entries_r
.done:  ret

; other_panel: BX -> the other panel, SI -> its entries
other_panel:
        mov     bx, panel_r
        mov     si, entries_r
        cmp     byte [active], 0
        je      .done
        mov     bx, panel_l
        mov     si, entries_l
.done:  ret

; entry_text: DI -> a record -> SI = the name as the user knows it
entry_text:
        mov     si, di
        cmp     byte [di+E_LONG], 0
        je      .done
        add     si, E_LONG
.done:  ret

; cur_entry: DI -> the record under the cursor.  CF=1 if the panel is empty
cur_entry:
        call    cur_panel
        cmp     word [bx+P_COUNT], 0
        je      .none
        mov     ax, [bx+P_CUR]
        mov     cx, ENTRY_SIZE
        mul     cx
        add     ax, si
        mov     di, ax
        clc
        ret
.none:  stc
        ret

read_both:
        mov     bx, panel_l
        mov     si, entries_l
        call    read_panel
        mov     bx, panel_r
        mov     si, entries_r
        call    read_panel
        ret

read_active:
        call    cur_panel
        jmp     read_panel

; read_panel: BX = panel, SI = its entries.  Lists the directory into the
;   records, directories first and then by name.  A directory that has gone
;   away is replaced by the root.
read_panel:
        push    bx
        push    si
        mov     si, entries_l                   ; the array belongs to the panel:
        cmp     bx, panel_l                     ;  never trust the caller's SI
        je      .have_entries
        mov     si, entries_r
.have_entries:
        lea     dx, [bx+P_PATH]
        mov     ah, 0x3B
        int     0x21
        jnc     .in_dir
        mov     word [bx+P_PATH], '\'           ; "\" and its NUL
        lea     dx, [bx+P_PATH]
        mov     ah, 0x3B
        int     0x21
.in_dir:
        mov     word [bx+P_COUNT], 0
        mov     di, si                          ; DI = the next record
        mov     dx, pat_all
        mov     cx, 0x37                        ; everything but volume labels
        mov     ah, 0x4E
        int     0x21
        jc      .listed
.next:  cmp     word [bx+P_COUNT], MAX_ENTRIES
        jae     .listed
        test    byte [dta+0x15], 0x08
        jnz     .skip
        cmp     byte [dta+0x1E], '.'
        jne     .take
        cmp     byte [dta+0x1F], 0              ; "." itself: never shown
        je      .skip
        cmp     word [bx+P_PATH], '\'           ; ".." in the root: nothing above
        je      .skip
.take:  push    si
        push    di
        mov     si, dta + 0x1E
        mov     cx, 13
        rep     movsb
        pop     di
        pop     si
        mov     al, [dta+0x15]
        mov     [di+E_ATTR], al
        mov     eax, [dta+0x1A]
        mov     [di+E_SIZE], eax
        ; the name as it really is, if the file has one
        push    si
        push    di
        push    dx
        mov     dx, long_buf
        mov     ah, 0xF3
        int     0x21
        pop     dx
        pop     di
        pop     si
        push    si
        push    di
        mov     si, long_buf
        add     di, E_LONG
        mov     cx, LONG_MAX - 1
.long_copy:
        lodsb
        mov     [di], al
        inc     di
        or      al, al
        jz      .long_done
        loop    .long_copy
        mov     byte [di], 0
.long_done:
        pop     di
        pop     si
        add     di, ENTRY_SIZE
        inc     word [bx+P_COUNT]
.skip:  mov     ah, 0x4F
        int     0x21
        jnc     .next
.listed:
        call    sort_entries
        mov     ax, [bx+P_CUR]
        cmp     ax, [bx+P_COUNT]
        jb      .cur_ok
        mov     ax, [bx+P_COUNT]
        dec     ax
        jns     .cur_set
        xor     ax, ax
.cur_set:
        mov     [bx+P_CUR], ax
.cur_ok:
        pop     si
        pop     bx
        ret

; sort_entries: BX = panel, SI = entries.  Insertion sort, directories first
sort_entries:
        pusha
        mov     si, entries_l
        cmp     bx, panel_l
        je      .own
        mov     si, entries_r
.own:   mov     cx, [bx+P_COUNT]
        cmp     cx, 2
        jb      .done
        dec     cx
        mov     di, si
        add     di, ENTRY_SIZE                  ; DI = the record to insert
.outer: mov     [sort_cx], cx
        mov     [sort_di], di
        ; the record aside
        push    si
        mov     si, di
        mov     di, tmp_entry
        mov     cx, ENTRY_SIZE
        rep     movsb
        pop     si
        mov     di, [sort_di]
        ; walk back while the record before should come after it
.inner: cmp     di, si
        je      .place
        push    di
        sub     di, ENTRY_SIZE
        call    entry_after                     ; CF=1: [DI] sorts after tmp
        pop     di
        jnc     .place
        push    si
        mov     si, di
        sub     si, ENTRY_SIZE
        mov     cx, ENTRY_SIZE
        rep     movsb                           ; move it down a slot
        pop     si
        sub     di, ENTRY_SIZE * 2              ; back to the slot before
        jmp     .inner
.place: push    si
        mov     si, tmp_entry
        mov     cx, ENTRY_SIZE
        rep     movsb
        pop     si
        mov     di, [sort_di]
        add     di, ENTRY_SIZE
        mov     cx, [sort_cx]
        loop    .outer
.done:  popa
        ret

; entry_after: CF=1 if the record at DI should come after tmp_entry
entry_after:
        push    si
        push    di
        mov     al, [di+E_ATTR]
        and     al, 0x10
        mov     ah, [tmp_entry+E_ATTR]
        and     ah, 0x10
        cmp     al, ah
        je      .same_kind
        or      al, al
        jz      .after                          ; a file after a directory
        jmp     .before
.same_kind:
        mov     si, tmp_entry
        cmp     byte [si+E_LONG], 0             ; sort on the shown name
        je      .tmp_short
        add     si, E_LONG
.tmp_short:
        cmp     byte [di+E_LONG], 0
        je      .cmp
        add     di, E_LONG
.cmp:   mov     al, [di]
        mov     ah, [si]
        cmp     al, ah
        ja      .after
        jb      .before
        or      al, al
        jz      .before
        inc     si
        inc     di
        jmp     .cmp
.after: pop     di
        pop     si
        stc
        ret
.before:
        pop     di
        pop     si
        clc
        ret

; ---------------------------------------------------------------- drawing
draw_all:
        call    draw_title
        mov     byte [d_col], 0
        call    draw_frame
        mov     byte [d_col], 40
        call    draw_frame
        call    draw_left
        call    draw_right
        call    draw_keys
        call    clear_msg
        ret

draw_active:
        cmp     byte [active], 0
        jne     draw_right
draw_left:
        mov     bx, panel_l
        mov     si, entries_l
        mov     byte [d_col], 0
        mov     byte [d_active], 0
        cmp     byte [active], 0
        jne     draw_panel
        mov     byte [d_active], 1
        jmp     draw_panel
draw_right:
        mov     bx, panel_r
        mov     si, entries_r
        mov     byte [d_col], 40
        mov     byte [d_active], 0
        cmp     byte [active], 1
        jne     draw_panel
        mov     byte [d_active], 1
        jmp     draw_panel

; draw_frame: a box from row 1 to row 22, 40 wide, at column [d_col]
draw_frame:
        pusha
        mov     bl, A_FRAME
        mov     dl, [d_col]
        mov     ah, 1
        mov     al, 0xDA
        call    put_char
        add     dl, 39
        mov     al, 0xBF
        call    put_char
        mov     ah, 22
        mov     al, 0xD9
        call    put_char
        mov     dl, [d_col]
        mov     al, 0xC0
        call    put_char
        inc     dl
        mov     cx, 38
        mov     al, 0xC4
        call    fill
        mov     ah, 1
        call    fill
        mov     cx, 20
        mov     ah, 2
.side:  mov     dl, [d_col]
        mov     al, 0xB3
        call    put_char
        add     dl, 39
        call    put_char
        inc     ah
        loop    .side
        popa
        ret

; draw_panel: BX = panel, SI = entries, [d_col] = frame column, [d_active]
draw_panel:
        pusha
        mov     [d_panel], bx
        mov     si, entries_l
        cmp     bx, panel_l
        je      .own_entries
        mov     si, entries_r
.own_entries:
        mov     [d_entries], si
        ; ---- the path, centred on the top edge ----
        mov     dl, [d_col]
        inc     dl
        mov     ah, 1
        mov     cx, 38
        mov     al, 0xC4
        mov     bl, A_FRAME
        call    fill
        mov     bx, [d_panel]
        lea     si, [bx+P_PATH]
        call    strlen                          ; CX = length
        cmp     cx, 36
        jbe     .fits
        mov     cx, 36
.fits:  mov     dl, [d_col]
        add     dl, 20
        mov     al, cl
        shr     al, 1
        sub     dl, al
        mov     bl, A_PATH
        cmp     byte [d_active], 0
        jne     .path_attr
        mov     bl, A_FRAME
.path_attr:
        mov     ah, 1
        call    put_text_n                      ; CX chars at AH,DL
        ; ---- keep the cursor in view ----
        mov     bx, [d_panel]
        mov     ax, [bx+P_CUR]
        cmp     ax, [bx+P_TOP]
        jae     .not_above
        mov     [bx+P_TOP], ax
.not_above:
        mov     ax, [bx+P_CUR]
        sub     ax, ROWS - 1
        cmp     ax, [bx+P_TOP]
        jle     .top_ok
        mov     [bx+P_TOP], ax
.top_ok:
        ; ---- the rows ----
        mov     ax, [bx+P_TOP]
        mov     [d_index], ax
        mov     byte [d_row], 2
.row:   mov     bx, [d_panel]
        mov     ax, [d_index]
        cmp     ax, [bx+P_COUNT]
        jae     .blank
        ; which colour
        mov     cx, ENTRY_SIZE
        mul     cx
        add     ax, [d_entries]
        mov     si, ax
        mov     byte [d_attr], A_FILE
        test    byte [si+E_ATTR], 0x10
        jz      .kind_set
        mov     byte [d_attr], A_DIR
.kind_set:
        cmp     byte [d_active], 0
        je      .colour_set
        mov     ax, [d_index]
        cmp     ax, [bx+P_CUR]
        jne     .colour_set
        mov     byte [d_attr], A_CUR
.colour_set:
        mov     bl, [d_attr]
        mov     ah, [d_row]
        mov     dl, [d_col]
        inc     dl
        mov     cx, 38
        mov     al, ' '
        call    fill
        add     dl, 1
        mov     cx, 12
        push    si
        cmp     byte [si+E_LONG], 0
        je      .show_name
        add     si, E_LONG
        mov     cx, 25                          ; a long name gets the room
.show_name:
        call    put_text_n
        pop     si
        add     dl, 16
        test    byte [si+E_ATTR], 0x10
        jz      .size
        push    si
        mov     si, str_dir
        mov     cx, 7
        call    put_text_n
        pop     si
        jmp     .row_done
.size:  mov     eax, [si+E_SIZE]
        call    put_size                        ; 8 wide at AH,DL
        jmp     .row_done
.blank: mov     ah, [d_row]
        mov     dl, [d_col]
        inc     dl
        mov     cx, 38
        mov     al, ' '
        mov     bl, A_FILE
        call    fill
.row_done:
        inc     word [d_index]
        inc     byte [d_row]
        cmp     byte [d_row], 2 + ROWS
        jb      .row
        popa
        ret

; draw_title: the top line
draw_title:
        pusha
        xor     ah, ah
        xor     dl, dl
        mov     cx, 80
        mov     al, ' '
        mov     bl, A_PATH
        call    fill
        mov     si, str_title
        mov     dl, 2
        call    put_text
        mov     si, str_title_r
        mov     dl, 57
        call    put_text
        popa
        ret

; draw_keys: the bottom line
draw_keys:
        pusha
        mov     ah, 24
        xor     dl, dl
        mov     cx, 80
        mov     al, ' '
        mov     bl, A_KEYNUM
        call    fill
        mov     si, keybar
.next:  cmp     byte [si], 0
        je      .done
        mov     cx, 2
        mov     bl, A_KEYNUM
        call    put_text_n                      ; the number
        add     si, 2
        add     dl, 2
        mov     cx, 6
        mov     bl, A_KEY
        call    put_text_n                      ; the word
        add     si, 6
        add     dl, 6
        jmp     .next
.done:  popa
        ret

clear_msg:
        pusha
        mov     ah, 23
        xor     dl, dl
        mov     cx, 80
        mov     al, ' '
        mov     bl, A_KEYNUM
        call    fill
        popa
        ret

; message: DS:SI = text on the message line
message:
        pusha
        call    clear_msg
        mov     ah, 23
        mov     dl, 1
        mov     bl, A_MSG
        call    put_text
        popa
        ret

; message_wait: show it and wait for a key
message_wait:
        call    message
        call    get_key
        call    clear_msg
        ret

; ---------------------------------------------------------------- screen
; put_char: AL at row AH, column DL, attribute BL
put_char:
        push    di
        push    ax
        call    scr_addr
        mov     ah, bl
        mov     [fs:di], ax
        pop     ax
        pop     di
        ret

; fill: CX copies of AL at row AH, column DL, attribute BL
fill:
        push    di
        push    cx
        push    ax
        call    scr_addr
        mov     ah, bl
.n:     mov     [fs:di], ax
        add     di, 2
        loop    .n
        pop     ax
        pop     cx
        pop     di
        ret

; put_text: DS:SI NUL-terminated at row AH, column DL, attribute BL
put_text:
        push    di
        push    si
        push    ax
        call    scr_addr
        mov     ah, bl
.n:     lodsb
        or      al, al
        jz      .done
        mov     [fs:di], ax
        add     di, 2
        jmp     .n
.done:  pop     ax
        pop     si
        pop     di
        ret

; put_text_n: CX characters of DS:SI at row AH, column DL, attribute BL;
;   stops at a NUL and pads with spaces
put_text_n:
        push    di
        push    si
        push    cx
        push    ax
        call    scr_addr
        mov     ah, bl
.n:     jcxz    .done
        mov     al, [si]
        or      al, al
        jnz     .have
        mov     al, ' '
        dec     si
.have:  inc     si
        mov     [fs:di], ax
        add     di, 2
        dec     cx
        jmp     .n
.done:  pop     ax
        pop     cx
        pop     si
        pop     di
        ret

; put_size: EAX right-aligned in 8 columns at row [d_row], column DL, attr BL
put_size:
        pusha
        push    dx
        mov     di, numbuf + 12
        mov     byte [di], 0
        mov     ecx, 10
        cmp     eax, 99999999
        jbe     .digits
        shr     eax, 10
        dec     di
        mov     byte [di], 'K'
.digits:
        xor     edx, edx
        div     ecx
        dec     di
        add     dl, '0'
        mov     [di], dl
        or      eax, eax
        jnz     .digits
        mov     si, di
        call    strlen                          ; CX = length
        pop     dx
        mov     al, 8
        sub     al, cl
        add     dl, al
        mov     ah, [d_row]
        call    put_text
        popa
        ret

; scr_addr: DI = screen offset of row AH, column DL
scr_addr:
        push    ax
        push    dx
        movzx   di, ah
        imul    di, 80
        movzx   dx, dl
        add     di, dx
        shl     di, 1
        pop     dx
        pop     ax
        ret

; strlen: CX = length of DS:SI
strlen:
        push    si
        xor     cx, cx
.n:     cmp     byte [si], 0
        je      .done
        inc     si
        inc     cx
        jmp     .n
.done:  pop     si
        ret

get_key:
        xor     ah, ah
        int     0x16
        ret

; ---------------------------------------------------------------- input
; input_line: DS:SI = prompt, DS:DI = buffer (may hold a starting value,
;   up to 63 characters).  CF=1 if Esc was pressed.
input_line:
        push    si
        push    di
        mov     [in_buf], di
        call    clear_msg
        mov     ah, 23
        mov     dl, 1
        mov     bl, A_MSG
        call    put_text
        call    strlen
        mov     [in_col], cl
        inc     byte [in_col]
        inc     byte [in_col]
        mov     si, di
        call    strlen
        mov     [in_len], cx
.show:  mov     ah, 23
        mov     dl, [in_col]
        mov     si, [in_buf]
        mov     cx, 58
        mov     bl, A_INPUT
        call    put_text_n
        ; the cursor
        mov     dl, [in_col]
        add     dl, [in_len]
        mov     dh, 23
        xor     bh, bh
        mov     ah, 0x02
        int     0x10
.key:   call    get_key
        cmp     al, 27
        je      .esc
        cmp     al, 13
        je      .ok
        cmp     al, 8
        je      .back
        cmp     al, 32
        jb      .key
        cmp     word [in_len], 63
        jae     .key
        mov     di, [in_buf]
        add     di, [in_len]
        mov     [di], al
        mov     byte [di+1], 0
        inc     word [in_len]
        jmp     .show
.back:  cmp     word [in_len], 0
        je      .key
        dec     word [in_len]
        mov     di, [in_buf]
        add     di, [in_len]
        mov     byte [di], 0
        jmp     .show
.ok:    call    hide_cursor
        call    clear_msg
        pop     di
        pop     si
        clc
        ret
.esc:   call    hide_cursor
        call    clear_msg
        pop     di
        pop     si
        stc
        ret

hide_cursor:
        mov     dx, 0x1900                      ; row 25: off the screen
        xor     bh, bh
        mov     ah, 0x02
        int     0x10
        ret

; ask_yes: DS:SI = question.  CF=0 for Y
ask_yes:
        call    message
        call    get_key
        call    clear_msg
        cmp     al, 'y'
        je      .yes
        cmp     al, 'Y'
        je      .yes
        stc
        ret
.yes:   clc
        ret

; ---------------------------------------------------------------- paths
; build_path: DI = buffer, BX = panel, SI = name -> the full path in DI
build_path:
        push    si
        push    di
        push    cx
        mov     [bp_name], si
        lea     si, [bx+P_PATH]
        call    strlen
        rep     movsb
        cmp     byte [di-1], '\'
        je      .sep_ok
        mov     al, '\'
        stosb
.sep_ok:
        mov     si, [bp_name]
.copy:  lodsb
        stosb
        or      al, al
        jnz     .copy
        pop     cx
        pop     di
        pop     si
        ret

; go_parent: the active panel goes up one directory
go_parent:
        call    cur_panel
        cmp     word [bx+P_PATH], '\'           ; "\" and NUL: the root
        je      .done
        lea     si, [bx+P_PATH]
        call    strlen
        lea     di, [bx+P_PATH]
        add     di, cx
.back:  dec     di
        cmp     byte [di], '\'
        jne     .back
        cmp     di, si
        jne     .cut
        inc     di                              ; keep the root's backslash
.cut:   mov     byte [di], 0
        mov     word [bx+P_CUR], 0
        mov     word [bx+P_TOP], 0
        call    read_panel
        call    draw_active
.done:  ret

; enter_dir: DI -> a directory record: the active panel goes into it
enter_dir:
        call    cur_panel
        cmp     word [di+E_NAME], ".."
        je      go_parent
        mov     [ed_rec], di
        lea     si, [bx+P_PATH]
        call    strlen                          ; CX = how long the path is
        lea     di, [bx+P_PATH]
        add     di, cx
        jcxz    .sep_ok
        cmp     byte [di-1], 0x5C
        je      .sep_ok
        mov     byte [di], 0x5C
        inc     di
.sep_ok:
        push    di
        mov     di, [ed_rec]
        call    entry_text                      ; SI = the name as shown
        pop     di
        mov     cx, LONG_MAX
.copy:  lodsb
        mov     [di], al
        inc     di
        or      al, al
        jz      .copied
        loop    .copy
        mov     byte [di], 0
.copied:
        mov     word [bx+P_CUR], 0
        mov     word [bx+P_TOP], 0
        call    read_panel
        call    draw_active
        ret

; extension_of: DI -> record -> SI -> the 3 characters after the dot
;   (or an empty string)
extension_of:
        mov     si, di
.n:     cmp     byte [si], 0
        je      .none
        cmp     byte [si], '.'
        je      .dot
        inc     si
        jmp     .n
.dot:   inc     si
        ret
.none:  mov     si, str_empty
        ret

; ext_is: SI -> extension, DI -> 3 upper-case letters.  ZF=1 if equal
ext_is:
        push    si
        push    di
        mov     cx, 3
.n:     mov     al, [si]
        cmp     al, 'a'
        jb      .up
        cmp     al, 'z'
        ja      .up
        sub     al, 32
.up:    cmp     al, [di]
        jne     .done
        inc     si
        inc     di
        loop    .n
        xor     al, al                          ; ZF = 1
.done:  pop     di
        pop     si
        ret

; ---------------------------------------------------------------- actions
do_enter:
        call    cur_entry
        jc      .done
        test    byte [di+E_ATTR], 0x10
        jnz     enter_dir
        push    di
        call    entry_text
        mov     di, si
        call    extension_of                    ; SI -> extension
        pop     di
        push    di
        mov     di, ext_com
        call    ext_is
        je      .run
        mov     di, ext_exe
        call    ext_is
        je      .run
        mov     di, ext_n32
        call    ext_is
        je      .run
        mov     di, ext_bat
        call    ext_is
        je      .batch
        mov     di, ext_mp3
        call    ext_is
        je      .music
        mov     di, ext_wav
        call    ext_is
        je      .music
        pop     di
        jmp     do_view
.run:   pop     di
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        mov     byte [tail_buf], 0
        mov     word [tail_buf+1], 0x000D
        mov     si, path_a
        call    run_and_return
        jnc     .done
        mov     si, msg_cant_run
        call    message_wait
.done:  ret
.batch: pop     di
        jmp     run_batch
.music: pop     di
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        ; the tail: " \MUSIC\SONG.MP3"
        mov     si, path_a
        call    strlen
        mov     [tail_buf], cl
        inc     byte [tail_buf]
        mov     di, tail_buf + 1
        mov     al, ' '
        stosb
        rep     movsb
        mov     al, 13
        stosb
        mov     si, str_player
        call    run_and_return
        ret

; run_and_return: DS:SI = program path, tail_buf = its command tail.
;   Runs it standing in the active panel's directory, then puts the screen
;   back and rereads both panels.  CF=1 if it could not be started.
run_and_return:
        push    si
        call    cur_panel
        lea     dx, [bx+P_PATH]
        mov     ah, 0x3B
        int     0x21
        pop     si
        mov     ax, 0x0003
        int     0x10
        mov     [save_ss], ss
        mov     [save_sp], sp
        mov     dx, si
        mov     bx, exec_block
        mov     word [exec_block+2], tail_buf
        mov     [exec_block+4], cs
        mov     [exec_block+8], cs
        mov     [exec_block+12], cs
        mov     ax, 0x4B00
        int     0x21
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ax, VIDEO
        mov     fs, ax
        cli
        mov     ss, [save_ss]
        mov     sp, [save_sp]
        sti
        pushf
        pop     ax
        mov     [exec_cf], ax
        mov     ah, 0x0F                        ; the video mode now
        int     0x10
        cmp     al, 3
        je      .mode_ok
        mov     ax, 0x0003
        int     0x10
.mode_ok:
        mov     dx, dta                         ; a child may have moved it
        mov     ah, 0x1A
        int     0x21
        call    read_both
        call    draw_all
        test    word [exec_cf], 1
        jnz     .failed
        clc
        ret
.failed:
        stc
        ret

; run_batch: DI -> the .BAT record.  Runs its lines, the simple way.
run_batch:
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        mov     si, path_a
        call    load_file                       ; -> iobuf, CX = bytes
        jc      .done
        mov     [bat_end], cx
        add     word [bat_end], iobuf
        mov     word [bat_pos], iobuf
.line:  mov     si, [bat_pos]
        cmp     si, [bat_end]
        jae     .done
        ; copy the line to line_buf, upper-casing as we go
        mov     di, line_buf
        xor     cx, cx
.copy:  cmp     si, [bat_end]
        jae     .eol
        lodsb
        cmp     al, 13
        je      .copy
        cmp     al, 10
        je      .eol
        cmp     cx, 78
        jae     .copy
        cmp     al, 'a'
        jb      .store
        cmp     al, 'z'
        ja      .store
        sub     al, 32
.store: stosb
        inc     cx
        jmp     .copy
.eol:   mov     byte [di], 0
        mov     [bat_pos], si
        mov     si, line_buf
.lead:  cmp     byte [si], ' '
        jne     .lead_done
        inc     si
        jmp     .lead
.lead_done:
        cmp     byte [si], '@'
        jne     .not_at
        inc     si
.not_at:
        cmp     byte [si], 0
        je      .line
        cmp     word [si], "EC"                 ; ECHO
        je      .line
        cmp     word [si], "RE"                 ; REM
        je      .line
        cmp     word [si], "PA"                 ; PAUSE
        je      .pause
        cmp     word [si], "CD"
        je      .cd
        call    run_word                        ; a program, with its tail
        jmp     .line
.pause: call    get_key
        jmp     .line
.cd:    add     si, 2
.cd_sp: cmp     byte [si], ' '
        jne     .cd_go
        inc     si
        jmp     .cd_sp
.cd_go: mov     dx, si
        mov     ah, 0x3B
        int     0x21
        jmp     .line
.done:  call    read_both
        call    draw_all
        ret

; run_word: SI -> "NAME args": try NAME, NAME.COM, NAME.EXE, NAME.N32
run_word:
        mov     di, path_a
.name:  lodsb
        cmp     al, ' '
        je      .name_done
        or      al, al
        jz      .name_done
        stosb
        jmp     .name
.name_done:
        mov     byte [di], 0
        mov     [rw_ext], di                    ; where an extension would go
        dec     si
        ; the tail: the rest of the line
        push    si
        call    strlen
        mov     [tail_buf], cl
        mov     di, tail_buf + 1
        rep     movsb
        mov     al, 13
        stosb
        pop     si
        mov     si, path_a
        call    run_and_return
        jnc     .ran
        mov     si, ext_tries
.try:   cmp     byte [si], 0
        je      .none
        push    si
        mov     di, [rw_ext]
        mov     cx, 5
        rep     movsb
        mov     si, path_a
        call    run_and_return
        pop     si
        jnc     .ran
        add     si, 5
        jmp     .try
.ran:   ret
.none:  mov     si, msg_cant_run
        call    message_wait
        ret

; load_file: DS:SI = path -> iobuf, CX = bytes read (up to IOBUF_SIZE)
load_file:
        mov     dx, si
        mov     ax, 0x3D00
        int     0x21
        jc      .fail
        mov     bx, ax
        mov     cx, IOBUF_SIZE
        mov     dx, iobuf
        mov     ah, 0x3F
        int     0x21
        push    ax
        mov     ah, 0x3E
        int     0x21
        pop     cx
        clc
        ret
.fail:  mov     si, msg_cant_open
        call    message_wait
        stc
        ret

do_view:
        call    cur_entry
        jc      .done
        test    byte [di+E_ATTR], 0x10
        jnz     .done
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        mov     si, path_a
        call    load_file
        jc      .done
        ; ---- index the lines ----
        mov     [view_bytes], cx
        mov     word [view_lines], 0
        mov     word [view_top], 0
        mov     si, iobuf
        mov     di, line_tab
        mov     cx, [view_bytes]
        add     cx, iobuf                       ; CX = end
        mov     [di], si
        add     di, 2
        inc     word [view_lines]
.scan:  cmp     si, cx
        jae     .indexed
        lodsb
        cmp     al, 10
        jne     .scan
        cmp     si, cx
        jae     .indexed
        cmp     word [view_lines], MAX_LINES
        jae     .indexed
        mov     [di], si
        add     di, 2
        inc     word [view_lines]
        jmp     .scan
.indexed:
        mov     [di], cx                        ; one past the last line
.draw:  call    view_draw
        call    get_key
        cmp     al, 27
        je      .leave
        cmp     ah, 0x3D
        je      .leave
        cmp     ah, 0x44
        je      .leave
        cmp     ah, 0x48
        je      .up
        cmp     ah, 0x50
        je      .dn
        cmp     ah, 0x49
        je      .pgup
        cmp     ah, 0x51
        je      .pgdn
        cmp     ah, 0x47
        je      .home
        cmp     ah, 0x4F
        je      .end
        jmp     .draw
.up:    cmp     word [view_top], 0
        je      .draw
        dec     word [view_top]
        jmp     .draw
.dn:    mov     ax, [view_top]
        inc     ax
        cmp     ax, [view_lines]
        jae     .draw
        mov     [view_top], ax
        jmp     .draw
.pgup:  sub     word [view_top], 22
        jns     .draw
        mov     word [view_top], 0
        jmp     .draw
.pgdn:  mov     ax, [view_top]
        add     ax, 22
        cmp     ax, [view_lines]
        jae     .end
        mov     [view_top], ax
        jmp     .draw
.home:  mov     word [view_top], 0
        jmp     .draw
.end:   mov     ax, [view_lines]
        sub     ax, 22
        jns     .set_end
        xor     ax, ax
.set_end:
        mov     [view_top], ax
        jmp     .draw
.leave: call    draw_all
.done:  ret

; view_draw: the file from line [view_top]
view_draw:
        pusha
        ; the title line
        mov     ah, 0
        xor     dl, dl
        mov     cx, 80
        mov     al, ' '
        mov     bl, A_PATH
        call    fill
        mov     si, path_a
        mov     dl, 1
        call    put_text
        mov     byte [d_row], 1
        mov     ax, [view_top]
        mov     [d_index], ax
.row:   mov     ah, [d_row]
        xor     dl, dl
        mov     cx, 80
        mov     al, ' '
        mov     bl, A_VIEW
        call    fill
        mov     ax, [d_index]
        cmp     ax, [view_lines]
        jae     .next
        shl     ax, 1
        add     ax, line_tab
        mov     bx, ax
        mov     si, [bx]
        mov     cx, [bx+2]
        sub     cx, si                          ; CX = the line's bytes
        mov     di, line_buf
        xor     dx, dx                          ; DX = characters kept
.ch:    jcxz    .line_done
        lodsb
        dec     cx
        cmp     al, 13
        je      .ch
        cmp     al, 10
        je      .ch
        cmp     al, 9
        jne     .keep
        mov     al, ' '
.keep:  cmp     al, 32
        jae     .ok
        mov     al, '.'
.ok:    cmp     dx, 80
        jae     .ch
        stosb
        inc     dx
        jmp     .ch
.line_done:
        mov     byte [di], 0
        mov     si, line_buf
        mov     ah, [d_row]
        xor     dl, dl
        mov     bl, A_VIEW
        call    put_text
.next:  inc     word [d_index]
        inc     byte [d_row]
        cmp     byte [d_row], 24
        jb      .row
        mov     ah, 24
        xor     dl, dl
        mov     cx, 80
        mov     al, ' '
        mov     bl, A_KEY
        call    fill
        mov     si, str_view_keys
        mov     dl, 1
        call    put_text
        popa
        ret

do_copy:
        call    cur_entry
        jc      .done
        test    byte [di+E_ATTR], 0x10
        jnz     .is_dir
        call    cur_panel
        push    di
        call    entry_text
        mov     di, path_a
        call    build_path
        pop     di
        call    other_panel
        push    di
        call    entry_text
        mov     di, path_b
        call    build_path
        pop     di
        ; the same place twice is not a copy
        mov     si, path_a
        mov     di, path_b
        call    str_equal
        je      .same
        mov     si, msg_copying
        call    message
        mov     dx, path_a
        mov     ax, 0x3D00
        int     0x21
        jc      .cant_open
        mov     [h_in], ax
        mov     dx, path_b
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .cant_create
        mov     [h_out], ax
.chunk: mov     bx, [h_in]
        mov     cx, IOBUF_SIZE
        mov     dx, iobuf
        mov     ah, 0x3F
        int     0x21
        jc      .write_fail
        or      ax, ax
        jz      .copied
        mov     cx, ax
        mov     bx, [h_out]
        mov     dx, iobuf
        mov     ah, 0x40
        int     0x21
        jc      .write_fail
        cmp     ax, cx
        jne     .write_fail
        jmp     .chunk
.copied:
        call    close_both
        call    read_both
        call    draw_all
.done:  ret
.write_fail:
        call    close_both
        mov     si, msg_copy_failed
        call    message_wait
        call    read_both
        call    draw_all
        ret
.cant_create:
        mov     bx, [h_in]
        mov     ah, 0x3E
        int     0x21
.cant_open:
        mov     si, msg_copy_failed
        call    message_wait
        ret
.same:  mov     si, msg_same_dir
        call    message_wait
        ret
.is_dir:
        mov     si, msg_no_dir_copy
        call    message_wait
        ret

close_both:
        mov     bx, [h_in]
        mov     ah, 0x3E
        int     0x21
        mov     bx, [h_out]
        mov     ah, 0x3E
        int     0x21
        ret

; str_equal: DS:SI and DS:DI, case-insensitively.  ZF=1 if equal
str_equal:
        push    si
        push    di
.n:     mov     al, [si]
        mov     ah, [di]
        call    upcase_al
        xchg    al, ah
        call    upcase_al
        cmp     al, ah
        jne     .done
        or      al, al
        jz      .done
        inc     si
        inc     di
        jmp     .n
.done:  pop     di
        pop     si
        ret

upcase_al:
        cmp     al, 'a'
        jb      .d
        cmp     al, 'z'
        ja      .d
        sub     al, 32
.d:     ret

do_move:
        call    cur_entry
        jc      .done
        call    cur_panel
        push    di
        call    entry_text
        mov     di, path_a
        call    build_path
        pop     di
        ; the suggestion: the same name in the other panel
        call    other_panel
        push    di
        call    entry_text
        mov     di, path_b
        call    build_path
        pop     di
        mov     si, msg_move_to
        mov     di, path_b
        call    input_line
        jc      .done
        mov     dx, path_a
        mov     di, path_b
        push    ds
        pop     es
        mov     ah, 0x56
        int     0x21
        jc      .failed
        call    read_both
        call    draw_all
.done:  ret
.failed:
        mov     si, msg_move_failed
        call    message_wait
        ret

do_mkdir:
        mov     byte [name_buf], 0
        mov     si, msg_mkdir
        mov     di, name_buf
        call    input_line
        jc      .done
        cmp     byte [name_buf], 0
        je      .done
        call    cur_panel
        mov     si, name_buf
        mov     di, path_a
        call    build_path
        mov     dx, path_a
        mov     ah, 0x39
        int     0x21
        jc      .failed
        call    read_both
        call    draw_all
.done:  ret
.failed:
        mov     si, msg_mkdir_failed
        call    message_wait
        ret

do_delete:
        call    cur_entry
        jc      .done
        cmp     word [di+E_NAME], ".."
        je      .done
        ; "Delete NAME? (Y/N)"
        push    di
        mov     si, msg_delete
        mov     di, line_buf
        call    copy_str
        pop     si
        push    si
        push    di
        mov     di, si
        call    entry_text
        pop     di
        call    copy_str
        mov     si, msg_yn
        call    copy_str
        mov     si, line_buf
        call    ask_yes
        pop     di
        jc      .done
        call    cur_panel
        push    di
        call    entry_text
        mov     di, path_a
        call    build_path
        pop     di
        mov     dx, path_a
        mov     ah, 0x41
        test    byte [di+E_ATTR], 0x10
        jz      .go
        mov     ah, 0x3A
.go:    int     0x21
        jc      .failed
        call    read_both
        call    draw_all
.done:  ret
.failed:
        mov     si, msg_delete_failed
        call    message_wait
        ret

; copy_str: DS:SI -> DS:DI, DI left at the NUL
copy_str:
.n:     lodsb
        stosb
        or      al, al
        jnz     .n
        dec     di
        ret

show_help:
        pusha
        mov     byte [d_row], 4
        mov     si, help_text
.line:  cmp     byte [si], 0
        je      .wait
        mov     ah, [d_row]
        mov     dl, 14
        mov     cx, 52
        mov     bl, A_HELP
        call    put_text_n
        call    strlen
        add     si, cx
        inc     si
        inc     byte [d_row]
        jmp     .line
.wait:  call    get_key
        call    draw_all
        popa
        ret

; ---------------------------------------------------------------- data
pat_all:        db "*.*", 0
str_dir:        db "<DIR>  ", 0
str_empty:      db 0
str_player:     db "\PLAYER.N32", 0
ext_com:        db "COM"
ext_exe:        db "EXE"
ext_n32:        db "N32"
ext_bat:        db "BAT"
ext_mp3:        db "MP3"
ext_wav:        db "WAV"
ext_tries:      db ".COM", 0, ".EXE", 0, ".N32", 0, 0
keybar:         db " 1Help  "
                db " 2      "
                db " 3View  "
                db " 4      "
                db " 5Copy  "
                db " 6RenMov"
                db " 7MkDir "
                db " 8Delete"
                db " 9      "
                db "10Quit  ", 0
str_view_keys:  db "Up/Down PgUp/PgDn Home/End scroll   Esc or F3 back", 0
str_title:      db "Ember File Manager", 0
str_title_r:    db "Tab switches   F1 help", 0
msg_cant_run:   db "That could not be started", 0
msg_cant_open:  db "That could not be opened", 0
msg_copying:    db "Copying...", 0
msg_copy_failed: db "The copy failed (is the disk full, or the name taken?)", 0
msg_same_dir:   db "Both panels show the same directory: nothing to copy to", 0
msg_no_dir_copy: db "Copying a whole directory is not done here yet", 0
msg_move_to:    db "Rename or move to:", 0
msg_move_failed: db "Could not rename or move it", 0
msg_mkdir:      db "Make directory:", 0
msg_mkdir_failed: db "Could not make that directory", 0
msg_delete:     db "Delete ", 0
msg_yn:         db "? (Y/N)", 0
msg_delete_failed: db "Could not delete it (a directory must be empty)", 0
help_text:
        db "                 Ember File Manager               ", 0
        db "                                                    ", 0
        db "  Tab        switch panels                          ", 0
        db "  Enter      open a directory, run a program,       ", 0
        db "             play music, or view a file             ", 0
        db "  Backspace  up one directory                       ", 0
        db "  F3 View    F5 Copy to the other panel             ", 0
        db "  F6 Rename or move       F7 Make directory         ", 0
        db "  F8 Delete               F10 or Esc  quit          ", 0
        db "  Ctrl+R     reread both panels                     ", 0
        db "                                                    ", 0
        db "               press any key to go back             ", 0
        db 0

section .bss
active:         resb 1
d_col:          resb 1
d_active:       resb 1
d_attr:         resb 1
d_row:          resb 1
d_panel:        resw 1
d_entries:      resw 1
d_index:        resw 1
sort_cx:        resw 1
sort_di:        resw 1
bp_name:        resw 1
in_buf:         resw 1
in_len:         resw 1
in_col:         resb 1
rw_ext:         resw 1
bat_pos:        resw 1
bat_end:        resw 1
view_bytes:     resw 1
view_lines:     resw 1
view_top:       resw 1
h_in:           resw 1
h_out:          resw 1
save_ss:        resw 1
save_sp:        resw 1
exec_cf:        resw 1
exec_block:     resb 14
numbuf:         resb 14
tail_buf:       resb 130
name_buf:       resb 66
path_a:         resb 130
path_b:         resb 130
line_buf:       resb 130
long_buf:       resb 84
ed_rec:         resw 1
tmp_entry:      resb ENTRY_SIZE
dta:            resb 48
panel_l:        resb PANEL_SIZE
panel_r:        resb PANEL_SIZE
entries_l:      resb MAX_ENTRIES * ENTRY_SIZE
entries_r:      resb MAX_ENTRIES * ENTRY_SIZE
line_tab:       resw MAX_LINES + 2
iobuf:          resb IOBUF_SIZE
