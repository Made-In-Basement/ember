"""Show long file names in the file manager, and use them.

Each panel entry gains room for the name as the user knows it.  The 8.3
name is still kept, because that is what a DOS program is given, but paths
are built from the long name so opening, copying and deleting all work on
the file the panel is actually showing.
"""


def patch(path, old, new):
    s = open(path, encoding='latin-1').read()
    assert s.count(old) == 1, (path, old[:70])
    open(path, 'w', encoding='latin-1').write(s.replace(old, new, 1))


P = 'programs/fm.asm'

patch(P, """MAX_ENTRIES     equ 256
ENTRY_SIZE      equ 20                          ; name[13] attr size[4] pad[2]
E_NAME          equ 0
E_ATTR          equ 13
E_SIZE          equ 14""",
"""MAX_ENTRIES     equ 256
LONG_MAX        equ 32                          ; of a long name, what we keep
ENTRY_SIZE      equ 52                          ; name[13] attr size[4] long[32]
E_NAME          equ 0
E_ATTR          equ 13
E_SIZE          equ 14
E_LONG          equ 20""")
patch(P, """IOBUF_SIZE      equ 24576""", """IOBUF_SIZE      equ 16384""")

# read_panel: ask for the long name of each entry it takes
patch(P, """.take:  push    si
        mov     si, dta + 0x1E
        mov     cx, 13
        rep     movsb
        pop     si
        mov     al, [dta+0x15]
        mov     [di-13+E_ATTR], al
        mov     eax, [dta+0x1A]
        mov     [di-13+E_SIZE], eax
        add     di, ENTRY_SIZE - 13
        inc     word [bx+P_COUNT]""",
""".take:  push    si
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
        inc     word [bx+P_COUNT]""")

# sorting compares whatever name is shown
patch(P, """.same_kind:
        mov     si, tmp_entry
.cmp:   mov     al, [di]
        mov     ah, [si]""",
""".same_kind:
        mov     si, tmp_entry
        cmp     byte [si+E_LONG], 0             ; sort on the shown name
        je      .tmp_short
        add     si, E_LONG
.tmp_short:
        cmp     byte [di+E_LONG], 0
        je      .cmp
        add     di, E_LONG
.cmp:   mov     al, [di]
        mov     ah, [si]""")

# entry_text: DI -> record, returns SI -> the name to show and to build paths from
patch(P, """; cur_entry: DI -> the record under the cursor.  CF=1 if the panel is empty""",
"""; entry_text: DI -> a record -> SI = the name as the user knows it
entry_text:
        mov     si, di
        cmp     byte [di+E_LONG], 0
        je      .done
        add     si, E_LONG
.done:  ret

; cur_entry: DI -> the record under the cursor.  CF=1 if the panel is empty""")

# drawing: show the long name
patch(P, """        mov     al, ' '
        call    fill
        add     dl, 1
        mov     cx, 12
        push    si
        call    put_text_n                      ; the name
        pop     si""",
"""        mov     al, ' '
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
        pop     si""")

# every operation works on the shown name
for old, new in [
    ("""        test    byte [di+E_ATTR], 0x10
        jnz     enter_dir
        push    di
        call    extension_of                    ; SI -> extension
        pop     di""",
     """        test    byte [di+E_ATTR], 0x10
        jnz     enter_dir
        push    di
        call    entry_text
        mov     di, si
        call    extension_of                    ; SI -> extension
        pop     di"""),
    ("""        pop     di
        call    cur_panel
        mov     si, di
        mov     di, path_a
        call    build_path
        mov     byte [tail_buf], 0""",
     """        pop     di
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        mov     byte [tail_buf], 0"""),
    ("""        pop     di
        call    cur_panel
        mov     si, di
        mov     di, path_a
        call    build_path
        ; the tail: " \\MUSIC\\SONG.MP3\"""",
     """        pop     di
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        ; the tail: " \\MUSIC\\SONG.MP3\""""),
    ("""run_batch:
        call    cur_panel
        mov     si, di
        mov     di, path_a""",
     """run_batch:
        call    cur_panel
        call    entry_text
        mov     di, path_a"""),
    ("""        jnz     .done
        call    cur_panel
        mov     si, di
        mov     di, path_a
        call    build_path
        mov     si, path_a
        call    load_file""",
     """        jnz     .done
        call    cur_panel
        call    entry_text
        mov     di, path_a
        call    build_path
        mov     si, path_a
        call    load_file"""),
    ("""        jnz     .is_dir
        call    cur_panel
        mov     si, di
        push    di
        mov     di, path_a
        call    build_path
        pop     di
        call    other_panel
        mov     si, di
        mov     di, path_b
        call    build_path""",
     """        jnz     .is_dir
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
        pop     di"""),
    ("""do_move:
        call    cur_entry
        jc      .done
        call    cur_panel
        mov     si, di
        push    di
        mov     di, path_a
        call    build_path
        pop     di
        ; the suggestion: the same name in the other panel
        call    other_panel
        mov     si, di
        mov     di, path_b
        call    build_path""",
     """do_move:
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
        pop     di"""),
    ("""        pop     di
        jc      .done
        call    cur_panel
        mov     si, di
        push    di
        mov     di, path_a
        call    build_path
        pop     di
        mov     dx, path_a
        mov     ah, 0x41""",
     """        pop     di
        jc      .done
        call    cur_panel
        push    di
        call    entry_text
        mov     di, path_a
        call    build_path
        pop     di
        mov     dx, path_a
        mov     ah, 0x41"""),
    ("""enter_dir:
        call    cur_panel
        cmp     word [di+E_NAME], "..\"""",
     """enter_dir:
        call    cur_panel
        cmp     word [di+E_NAME], "..\""""),
]:
    patch(P, old, new)

# entering a directory: rewritten to append the name the panel shows
old_enter = open(P, encoding='latin-1').read()
i = old_enter.index("enter_dir:")
j = old_enter.index("; extension_of:")
new_enter = """enter_dir:
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

"""
open(P, 'w', encoding='latin-1').write(old_enter[:i] + new_enter + old_enter[j:])

# the delete question shows the real name
patch(P, """        push    di
        mov     si, msg_delete
        mov     di, line_buf
        call    copy_str
        pop     si
        push    si
        call    copy_str""",
"""        push    di
        mov     si, msg_delete
        mov     di, line_buf
        call    copy_str
        pop     si
        push    si
        push    di
        mov     di, si
        call    entry_text
        pop     di
        call    copy_str""")

patch(P, """line_buf:       resb 130""", """line_buf:       resb 130
long_buf:       resb 84
ed_rec:         resw 1""")
print('the file manager shows and uses long names')
