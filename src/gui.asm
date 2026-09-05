; =============================================================================
;  gui.asm - the graphical shell: desktop, taskbar, Start menu, window manager
; -----------------------------------------------------------------------------
;  Started by the WIN command.  Everything is drawn with the primitives in
;  gfx.asm; the applications live in apps.asm.  FS = GUI_SEG while the shell
;  runs (scratch memory for directory listings and file text).
; =============================================================================

MAX_WIN         equ 4
WIN_FILEMAN     equ 0
WIN_NOTEPAD     equ 1
WIN_CALC        equ 2
WIN_ABOUT       equ 3
NUM_ICONS       equ 5
NUM_MENU        equ 6
MENU_SEP_AFTER  equ 3                   ; separator drawn after this item
DBLCLICK_TICKS  equ 9                   ; ~0.5 s

; window record
W_X             equ 0
W_Y             equ 2
W_W             equ 4
W_H             equ 6
W_OPEN          equ 8
W_SIZE          equ 10

; -----------------------------------------------------------------------------
; gui_main: DS:SI = arguments: none = auto (800x600 if possible),
;   "640" / "800" / "1024" = that VESA mode, "LOW" = 320x200 VGA
; -----------------------------------------------------------------------------
gui_main:
        call    gui_scratch_get                     ; room for its lists and text
        jc      gui_no_memory
        xor     ah, ah                          ; AH = mode selector
        mov     byte [gui_nomouse], 0
        mov     byte [mouse_force_bios], 0
        mov     al, [si]
        call    upcase
        cmp     al, 'N'                         ; NOMOUSE
        jne     .not_nomouse
        mov     byte [gui_nomouse], 1
.not_nomouse:
        cmp     al, 'B'                         ; BIOSMOUSE
        jne     .not_biosmouse
        mov     byte [mouse_force_bios], 1
.not_biosmouse:
        cmp     al, 'L'
        jne     .not_low
        mov     ah, 1
.not_low:
        cmp     al, '6'
        jne     .not_640
        mov     ah, 2
.not_640:
        cmp     al, '8'
        jne     .not_800
        mov     ah, 3
.not_800:
        cmp     al, '1'
        jne     .not_1024
        mov     ah, 4
.not_1024:
        mov     [gui_vmode], ah
        mov     al, ah
        call    gfx_init
        jc      .no_gfx
        mov     fs, [gui_seg]
        call    gui_layout
        call    gui_reset_state
        mov     byte [mouse_ok], 0
        cmp     byte [gui_nomouse], 0
        jne     .no_mouse
        call    mouse_init                      ; keyboard-only if this fails
.no_mouse:
        mov     ax, [scr_w]
        shr     ax, 1
        mov     [mouse_x], ax
        mov     ax, [scr_h]
        shr     ax, 1
        mov     [mouse_y], ax
        ; note the outcome in the boot log
        movzx   eax, word [scr_w]
        shl     eax, 16
        mov     ax, [scr_h]
        mov     si, msg_log_mode
        call    log_line
        movzx   eax, byte [mouse_fail_step]
        mov     si, msg_log_mouse
        call    log_line
        movzx   eax, byte [mouse_bios]
        mov     si, msg_log_mouse_bios
        call    log_line
        call    gui_redraw
        call    gui_loop
        call    cursor_hide
        call    mouse_stop
        call    gfx_done
        call    cls
        call    log_flush
        call    gui_scratch_put
        ret
.no_gfx:
        mov     si, msg_no_gfx
        call    puts
        call    gui_scratch_put
        ret

gui_no_memory:
        mov     si, msg_gui_no_mem
        call    puts
        ret

; -----------------------------------------------------------------------------
; gui_layout: derive metrics from the screen size, load default window rects
; -----------------------------------------------------------------------------
gui_layout:
        pusha
        movzx   ax, byte [font_h]
        add     ax, 4
        mov     [L_title_h], ax
        movzx   ax, byte [font_h]
        add     ax, 12
        mov     [L_task_h], ax
        mov     bx, [scr_h]
        sub     bx, ax
        mov     [L_task_y], bx
        movzx   ax, byte [font_h]
        add     ax, 6
        mov     [L_btn_h], ax
        movzx   ax, byte [font_h]
        add     ax, 6
        mov     [L_item_h], ax
        mov     si, layout_hi
        cmp     byte [gfx_mode], 1
        je      .copy
        mov     si, layout_lo
.copy:  lodsw
        mov     [L_start_w], ax
        lodsw
        mov     [L_taskbtn_w], ax
        lodsw
        mov     [L_tray_w], ax
        lodsw
        mov     [L_icon_step], ax
        lodsw
        mov     [L_menu_w], ax
        mov     di, windows
        mov     cx, MAX_WIN
.win:   lodsw
        call    .scale_x
        stosw
        lodsw
        call    .scale_y
        stosw
        lodsw
        call    .scale_x
        stosw
        lodsw
        call    .scale_y
        stosw
        mov     word [di], 0
        add     di, 2
        loop    .win
        popa
        ret
; the VESA table describes a 640x480 screen; stretch it to the real size
.scale_x:
        cmp     byte [gfx_mode], 0
        je      .same
        mul     word [scr_w]
        mov     bx, 640
        div     bx
.same:  ret
.scale_y:
        cmp     byte [gfx_mode], 0
        je      .same
        mul     word [scr_h]
        mov     bx, 480
        div     bx
        ret

; per-mode layout: start_w, taskbtn_w, tray_w, icon_step, menu_w, then window rects
layout_hi:
        dw 54, 110, 64, 72, 160
        dw 120, 40, 360, 300                    ; File Manager
        dw 200, 90, 420, 320                    ; Notepad
        dw 340, 120, 176, 236                   ; Calculator
        dw 160, 140, 330, 190                   ; About
layout_lo:
        dw 44, 64, 46, 46, 110
        dw 56, 8, 240, 150                      ; File Manager
        dw 36, 14, 270, 156                     ; Notepad
        dw 90, 10, 130, 150                     ; Calculator
        dw 24, 30, 270, 120                     ; About

gui_reset_state:
        mov     word [z_count], 0
        mov     byte [menu_open], 0
        mov     word [menu_sel], -1
        mov     word [drag_win], -1
        mov     word [sel_icon], -1
        mov     byte [gui_quit], 0
        mov     byte [cursor_visible], 0
        mov     byte [prev_buttons], 0
        mov     dword [last_click_ticks], 0
        mov     word [last_click_id], 0
        mov     byte [clock_minute], 0xFF
        ret

; =============================================================================
; main loop
; =============================================================================
gui_loop:
.loop:  cmp     byte [gui_quit], 0
        jne     .done
        call    kbhit
        jz      .no_key
        call    getkey
        call    gui_key
        jmp     .loop
.no_key:
        call    gui_poll_mouse
        call    gui_clock_tick
        sti
        hlt                                     ; sleep until the next interrupt
        jmp     .loop
.done:  ret

; -----------------------------------------------------------------------------
; gui_poll_mouse: apply accumulated motion, detect button transitions
; -----------------------------------------------------------------------------
gui_poll_mouse:
        pusha
        cmp     byte [mouse_ok], 0
        je      .ret
        cli
        mov     ax, [mouse_dx]
        mov     word [mouse_dx], 0
        mov     bx, [mouse_dy]
        mov     word [mouse_dy], 0
        mov     cl, [mouse_buttons]
        sti
        mov     [cur_buttons], cl
        or      ax, ax
        jnz     .moved
        or      bx, bx
        jz      .buttons
.moved:
        add     ax, [mouse_x]
        test    ax, ax
        jns     .x1
        xor     ax, ax
.x1:    cmp     ax, [scr_w]
        jb      .x2
        mov     ax, [scr_w]
        dec     ax
.x2:    add     bx, [mouse_y]
        test    bx, bx
        jns     .y1
        xor     bx, bx
.y1:    cmp     bx, [scr_h]
        jb      .y2
        mov     bx, [scr_h]
        dec     bx
.y2:    call    cursor_hide
        mov     [mouse_x], ax
        mov     [mouse_y], bx
        cmp     word [drag_win], -1
        je      .no_drag
        call    drag_update
.no_drag:
        cmp     byte [menu_open], 0
        je      .no_menu
        call    menu_hover
.no_menu:
        call    cursor_show
.buttons:
        mov     cl, [cur_buttons]
        mov     ch, [prev_buttons]
        mov     [prev_buttons], cl
        test    ch, 1
        jnz     .was_down
        test    cl, 1
        jz      .ret
        mov     ax, [mouse_x]
        mov     bx, [mouse_y]
        call    gui_click
        jmp     .ret
.was_down:
        test    cl, 1
        jnz     .ret
        call    gui_release
.ret:   popa
        ret

; -----------------------------------------------------------------------------
; gui_clock_tick: redraw the tray clock when the minute changes
; -----------------------------------------------------------------------------
gui_clock_tick:
        pusha
        call    rtc_read_time                   ; CH = hour, CL = minute
        cmp     cl, [clock_minute]
        je      .done
        mov     [clock_minute], cl
        call    cursor_hide
        call    draw_clock
        call    cursor_show
.done:  popa
        ret

; =============================================================================
; mouse cursor with save-under
; =============================================================================
cursor_show:
        pusha
        cmp     byte [mouse_ok], 0
        je      .ret
        cmp     byte [cursor_visible], 0
        jne     .ret
        mov     ax, [mouse_x]
        mov     bx, [mouse_y]
        mov     [cursor_sx], ax
        mov     [cursor_sy], bx
        mov     di, cursor_save
        mov     dx, CURSOR_H
.row:   push    ax
        mov     si, CURSOR_W
.px:    call    gfx_get_pixel
        mov     [di], cl
        inc     di
        inc     ax
        dec     si
        jnz     .px
        pop     ax
        inc     bx
        dec     dx
        jnz     .row
        mov     ax, [mouse_x]
        mov     bx, [mouse_y]
        mov     cx, CURSOR_W
        mov     dx, CURSOR_H
        mov     si, cursor_art
        call    gfx_draw_image
        mov     byte [cursor_visible], 1
.ret:   popa
        ret

cursor_hide:
        pusha
        cmp     byte [cursor_visible], 0
        je      .ret
        mov     ax, [cursor_sx]
        mov     bx, [cursor_sy]
        mov     si, cursor_save
        mov     dx, CURSOR_H
.row:   push    ax
        mov     di, CURSOR_W
.px:    mov     cl, [si]
        call    gfx_put_pixel
        inc     si
        inc     ax
        dec     di
        jnz     .px
        pop     ax
        inc     bx
        dec     dx
        jnz     .row
        mov     byte [cursor_visible], 0
.ret:   popa
        ret

; =============================================================================
; drawing
; =============================================================================
; gui_redraw: everything, back to front
gui_redraw:
        pusha
        call    cursor_hide
        call    draw_desktop
        call    draw_icons
        xor     si, si
.win:   cmp     si, [z_count]
        jae     .windows_done
        movzx   ax, byte [z_order+si]
        call    win_draw
        inc     si
        jmp     .win
.windows_done:
        call    draw_taskbar
        cmp     byte [menu_open], 0
        je      .no_menu
        call    draw_menu
.no_menu:
        call    cursor_show
        popa
        ret

; gui_redraw_top: redraw only the active window (it is on top, so this is safe)
gui_redraw_top:
        pusha
        call    gui_active
        cmp     ax, -1
        je      .done
        call    cursor_hide
        call    win_draw
        call    cursor_show
.done:  popa
        ret

draw_desktop:
        pusha
        mov     byte [pen], C_DESKTOP
        xor     ax, ax
        xor     bx, bx
        mov     cx, [scr_w]
        mov     dx, [L_task_y]
        call    gfx_fill_rect
        popa
        ret

; icon_pos: AX = icon index -> AX = x, BX = y of the 32x32 image
icon_pos:
        push    cx
        push    dx
        mov     cx, [L_task_y]
        sub     cx, 8
        xor     dx, dx
        push    ax
        mov     ax, cx
        div     word [L_icon_step]              ; AX = icons per column
        mov     cx, ax
        pop     ax
        xor     dx, dx
        div     cx                              ; AX = column, DX = row
        imul    ax, 100
        add     ax, 34
        push    ax
        mov     ax, dx
        mul     word [L_icon_step]
        add     ax, 12
        mov     bx, ax
        pop     ax
        pop     dx
        pop     cx
        ret

draw_icons:
        pusha
        xor     si, si
.next:  mov     ax, si
        call    icon_pos                        ; AX,BX = image position
        push    si
        shl     si, 2
        mov     cx, ICON_W
        mov     dx, ICON_H
        push    si
        mov     si, [icon_table+si]
        call    gfx_draw_image
        pop     si
        mov     byte [text_fg], C_WHITE
        mov     byte [text_bg], C_DESKTOP
        pop     di
        cmp     di, [sel_icon]
        jne     .label
        mov     byte [text_bg], C_NAVY
.label: push    si
        mov     si, [icon_table+si+2]
        sub     ax, 32
        add     bx, ICON_H + 3
        mov     cx, 96
        call    gfx_text_center
        pop     si
        mov     si, di
        inc     si
        cmp     si, NUM_ICONS
        jb      .next
        popa
        ret

icon_table:
        dw icon_computer, str_my_computer
        dw icon_notepad, str_notepad
        dw icon_calc, str_calculator
        dw icon_about, str_about_short
        dw icon_dos, str_dos_prompt

; icon_hit: AX,BX = point -> CX = icon index or -1
icon_hit:
        push    ax
        push    bx
        push    dx
        push    si
        mov     dx, ax
        mov     si, bx                          ; DX,SI = point
        xor     cx, cx
.next:  mov     ax, cx
        call    icon_pos                        ; AX,BX = icon origin
        sub     ax, 32                          ; label area is wider
        cmp     dx, ax
        jb      .miss
        add     ax, 96
        cmp     dx, ax
        jae     .miss
        cmp     si, bx
        jb      .miss
        movzx   ax, byte [font_h]
        add     ax, ICON_H + 4
        add     bx, ax
        cmp     si, bx
        jae     .miss
        jmp     .done
.miss:  inc     cx
        cmp     cx, NUM_ICONS
        jb      .next
        mov     cx, -1
.done:  pop     si
        pop     dx
        pop     bx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; taskbar
; -----------------------------------------------------------------------------
draw_taskbar:
        pusha
        mov     byte [pen], C_SILVER
        xor     ax, ax
        mov     bx, [L_task_y]
        mov     cx, [scr_w]
        mov     dx, [L_task_h]
        call    gfx_fill_rect
        mov     byte [pen], C_WHITE
        inc     bx
        call    gfx_hline
        ; Start button
        mov     ax, 2
        mov     bx, [L_task_y]
        add     bx, 3
        mov     cx, [L_start_w]
        mov     dx, [L_task_h]
        sub     dx, 6
        mov     byte [bevel], 0
        cmp     byte [menu_open], 0
        je      .start_face
        mov     byte [bevel], 1
.start_face:
        call    gfx_bevel
        call    draw_logo
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_SILVER
        mov     ax, 20
        mov     bx, [L_task_y]
        mov     dx, [L_task_h]
        movzx   cx, byte [font_h]
        sub     dx, cx
        shr     dx, 1
        add     bx, dx
        mov     si, str_start
        call    gfx_text
        ; task buttons
        mov     di, [L_start_w]
        add     di, 8
        xor     si, si
.task:  call    win_record                      ; SI = index -> BX = record
        cmp     word [bx+W_OPEN], 0
        je      .task_next
        mov     ax, di
        mov     bx, [L_task_y]
        add     bx, 3
        mov     cx, [L_taskbtn_w]
        mov     dx, [L_task_h]
        sub     dx, 6
        mov     byte [bevel], 0
        push    ax
        call    gui_active
        cmp     ax, si
        pop     ax
        jne     .task_face
        mov     byte [bevel], 1
.task_face:
        call    gfx_bevel
        add     ax, 6
        movzx   cx, byte [font_h]
        sub     dx, cx
        shr     dx, 1
        add     bx, dx
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_SILVER
        push    si
        shl     si, 1
        mov     si, [win_titles+si]
        mov     cx, [L_taskbtn_w]
        sub     cx, 12
        shr     cx, 3                           ; characters that fit
        call    text_len
        cmp     dx, cx
        jbe     .fits
        mov     dx, cx
.fits:  mov     cx, dx
        call    gfx_text_n
        pop     si
        add     di, [L_taskbtn_w]
        add     di, 3
.task_next:
        inc     si
        cmp     si, MAX_WIN
        jb      .task
        ; tray
        mov     ax, [scr_w]
        sub     ax, [L_tray_w]
        sub     ax, 2
        mov     bx, [L_task_y]
        add     bx, 3
        mov     cx, [L_tray_w]
        mov     dx, [L_task_h]
        sub     dx, 6
        mov     byte [bevel], 1
        call    gfx_bevel
        call    draw_clock
        popa
        ret

; draw_logo: the four coloured squares on the Start button
draw_logo:
        pusha
        mov     ax, 7
        mov     bx, [L_task_y]
        mov     dx, [L_task_h]
        sub     dx, 10
        shr     dx, 1
        add     bx, dx
        mov     cx, 4
        mov     dx, 4
        mov     byte [pen], C_RED
        call    gfx_fill_rect
        add     ax, 5
        mov     byte [pen], C_LIME
        call    gfx_fill_rect
        add     bx, 5
        mov     byte [pen], C_YELLOW
        call    gfx_fill_rect
        sub     ax, 5
        mov     byte [pen], C_BLUE
        call    gfx_fill_rect
        popa
        ret

draw_clock:
        pusha
        call    rtc_read_time                   ; CH = hour, CL = minute
        mov     di, clock_str
        mov     al, ch
        call    two_digits
        mov     byte [di], ':'
        inc     di
        mov     al, cl
        call    two_digits
        mov     byte [di], 0
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_SILVER
        mov     ax, [scr_w]
        sub     ax, [L_tray_w]
        sub     ax, 2
        mov     cx, [L_tray_w]
        mov     bx, [L_task_y]
        mov     dx, [L_task_h]
        push    cx
        movzx   cx, byte [font_h]
        sub     dx, cx
        shr     dx, 1
        pop     cx
        add     bx, dx
        mov     si, clock_str
        call    gfx_text_center
        popa
        ret

; two_digits: AL (0..99) -> two ASCII digits at DS:DI, DI += 2
two_digits:
        push    ax
        aam
        add     ax, 0x3030
        mov     [di], ah
        mov     [di+1], al
        add     di, 2
        pop     ax
        ret

; text_len: DS:SI NUL-terminated -> DX = length
text_len:
        push    si
        xor     dx, dx
.next:  cmp     byte [si], 0
        je      .done
        inc     si
        inc     dx
        jmp     .next
.done:  pop     si
        ret

; -----------------------------------------------------------------------------
; Start menu
; -----------------------------------------------------------------------------
; menu_rect: -> AX,BX,CX,DX = menu rectangle
menu_rect:
        mov     ax, [L_item_h]
        imul    ax, NUM_MENU
        add     ax, 14                          ; borders + separator
        mov     dx, ax
        mov     bx, [L_task_y]
        sub     bx, dx
        mov     ax, 2
        mov     cx, [L_menu_w]
        ret

; menu_item_rect: SI = item -> AX,BX,CX,DX = highlight rectangle of that item
menu_item_rect:
        call    menu_rect
        add     ax, 26                          ; past the band
        add     bx, 4
        sub     cx, 30
        mov     dx, [L_item_h]
        push    dx
        imul    dx, si
        add     bx, dx
        pop     dx
        cmp     si, MENU_SEP_AFTER
        jbe     .done
        add     bx, 6                           ; below the separator
.done:  ret

draw_menu:
        pusha
        call    menu_rect
        mov     byte [bevel], 0
        call    gfx_bevel
        ; the navy band with the name stacked vertically
        push    ax
        push    bx
        push    cx
        push    dx
        add     ax, 3
        add     bx, 3
        mov     cx, 22
        sub     dx, 6
        mov     byte [pen], C_NAVY
        call    gfx_fill_rect
        mov     byte [text_fg], C_WHITE
        mov     byte [text_bg], C_NAVY
        add     ax, 7
        add     bx, dx
        mov     si, str_band
        call    text_len                        ; DX = letters
        movzx   cx, byte [font_h]
        imul    cx, dx
        sub     bx, cx
        sub     bx, 2
        add     si, dx
        dec     si                              ; last letter first? no: draw top-down
        sub     si, dx
        inc     si
.band:  mov     cl, [si]
        or      cl, cl
        jz      .band_done
        call    gfx_char
        movzx   cx, byte [font_h]
        add     bx, cx
        inc     si
        jmp     .band
.band_done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ; items
        xor     si, si
.item:  call    menu_item_rect
        mov     byte [text_fg], C_BLACK
        mov     byte [text_bg], C_SILVER
        cmp     si, [menu_sel]
        jne     .plain
        mov     byte [pen], C_NAVY
        call    gfx_fill_rect
        mov     byte [text_fg], C_WHITE
        mov     byte [text_bg], C_NAVY
.plain: add     ax, 6
        add     bx, 3
        push    si
        shl     si, 1
        mov     si, [menu_items+si]
        call    gfx_text
        pop     si
        cmp     si, MENU_SEP_AFTER
        jne     .no_sep
        ; separator: grey then white line
        call    menu_item_rect
        add     bx, dx
        add     bx, 2
        mov     byte [pen], C_GRAY
        call    gfx_hline
        inc     bx
        mov     byte [pen], C_WHITE
        call    gfx_hline
.no_sep:
        inc     si
        cmp     si, NUM_MENU
        jb      .item
        popa
        ret

; menu_hit: AX,BX = point -> CX = item index, or -1 (outside the menu / on no item)
menu_hit:
        push    ax
        push    bx
        push    dx
        push    si
        push    di
        mov     di, ax
        mov     si, bx                          ; DI,SI = point
        xor     cx, cx
.next:  push    si
        mov     si, cx
        call    menu_item_rect
        pop     si
        cmp     di, ax
        jb      .miss
        add     ax, cx
        cmp     di, ax
        jae     .miss
        cmp     si, bx
        jb      .miss
        add     bx, dx
        cmp     si, bx
        jae     .miss
        jmp     .done
.miss:  inc     cx
        cmp     cx, NUM_MENU
        jb      .next
        mov     cx, -1
.done:  pop     di
        pop     si
        pop     dx
        pop     bx
        pop     ax
        ret

; menu_hover: highlight the item under the mouse (cursor already hidden)
menu_hover:
        pusha
        mov     ax, [mouse_x]
        mov     bx, [mouse_y]
        call    menu_hit
        cmp     cx, -1
        je      .done
        cmp     cx, [menu_sel]
        je      .done
        mov     [menu_sel], cx
        call    draw_menu
.done:  popa
        ret

; menu_action: CX = item
menu_action:
        cmp     cx, 4
        jb      .open
        je      .exit
        ; shut down
        call    cursor_hide
        call    mouse_stop
        call    gfx_done
        jmp     cmd_shutdown
.exit:  mov     byte [gui_quit], 1
        ret
.open:  mov     ax, cx
        call    win_open
        ret

menu_items:
        dw str_my_computer, str_notepad, str_calculator, str_about
        dw str_exit_dos, str_shutdown

; =============================================================================
; window manager
; =============================================================================
; win_record: SI = index -> BX = record address (preserves SI)
win_record:
        mov     bx, si
        imul    bx, W_SIZE
        add     bx, windows
        ret

; gui_active: AX = index of the active (topmost) window, or -1
gui_active:
        push    si
        mov     si, [z_count]
        or      si, si
        jz      .none
        movzx   ax, byte [z_order+si-1]
        pop     si
        ret
.none:  mov     ax, -1
        pop     si
        ret

; win_raise: AX = index -> move to the top of the z-order
win_raise:
        pusha
        call    z_remove
        mov     si, [z_count]
        mov     [z_order+si], al
        inc     word [z_count]
        popa
        ret

; z_remove: AX = index -> remove from the z-order if present
z_remove:
        pusha
        xor     si, si
.find:  cmp     si, [z_count]
        jae     .done
        cmp     [z_order+si], al
        je      .found
        inc     si
        jmp     .find
.found: mov     di, si
.shift: inc     si
        cmp     si, [z_count]
        jae     .shifted
        mov     cl, [z_order+si]
        mov     [z_order+di], cl
        inc     di
        jmp     .shift
.shifted:
        dec     word [z_count]
.done:  popa
        ret

; win_open: AX = index -> open (initialising the app if needed) and raise
win_open:
        pusha
        mov     si, ax
        call    win_record
        cmp     word [bx+W_OPEN], 0
        jne     .raise
        mov     word [bx+W_OPEN], 1
        shl     si, 1
        call    word [app_open+si]
        shr     si, 1
.raise: call    win_raise
        call    gui_redraw
        popa
        ret

; win_close: AX = index
win_close:
        pusha
        mov     si, ax
        call    win_record
        mov     word [bx+W_OPEN], 0
        call    z_remove
        call    gui_redraw
        popa
        ret

; win_client: AX = index -> cl_x/cl_y/cl_w/cl_h
win_client:
        pusha
        mov     si, ax
        call    win_record
        mov     ax, [bx+W_X]
        add     ax, 3
        mov     [cl_x], ax
        mov     ax, [bx+W_Y]
        add     ax, 3
        add     ax, [L_title_h]
        inc     ax
        mov     [cl_y], ax
        mov     ax, [bx+W_W]
        sub     ax, 6
        mov     [cl_w], ax
        mov     ax, [bx+W_H]
        sub     ax, 7
        sub     ax, [L_title_h]
        mov     [cl_h], ax
        popa
        ret

; win_draw: AX = index (frame, title bar, close button, then the app's client)
win_draw:
        pusha
        mov     si, ax
        mov     [draw_win], ax
        call    win_record
        mov     ax, [bx+W_X]
        push    bx
        mov     cx, [bx+W_W]
        mov     dx, [bx+W_H]
        mov     bx, [bx+W_Y]
        mov     byte [bevel], 0
        call    gfx_bevel
        ; title bar
        add     ax, 3
        add     bx, 3
        sub     cx, 6
        mov     dx, [L_title_h]
        mov     byte [pen], C_GRAY
        push    ax
        call    gui_active
        cmp     ax, si
        pop     ax
        jne     .bar
        mov     byte [pen], C_NAVY
.bar:   call    gfx_fill_rect
        mov     dl, [pen]
        mov     [text_bg], dl
        mov     byte [text_fg], C_WHITE
        push    ax
        push    bx
        push    cx
        add     ax, 4
        add     bx, 2
        push    si
        shl     si, 1
        mov     si, [win_titles+si]
        call    gfx_text
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ; close button
        add     ax, cx
        sub     ax, 18
        add     bx, 2
        mov     cx, 16
        mov     dx, [L_title_h]
        sub     dx, 4
        mov     byte [bevel], 0
        call    gfx_bevel
        ; the X glyph: two 2-pixel-wide diagonals, 7 rows
        push    ax
        push    bx
        push    si
        push    di
        add     ax, 4
        sub     dx, 7
        shr     dx, 1
        add     bx, dx                          ; centre vertically
        mov     si, ax                          ; SI = left diagonal x
        mov     di, ax
        add     di, 7                           ; DI = right diagonal x
        mov     cl, C_BLACK
        mov     dx, 7
.x:     mov     ax, si
        call    gfx_put_pixel
        inc     ax
        call    gfx_put_pixel
        mov     ax, di
        call    gfx_put_pixel
        dec     ax
        call    gfx_put_pixel
        inc     si
        dec     di
        inc     bx
        dec     dx
        jnz     .x
        pop     di
        pop     si
        pop     bx
        pop     ax
        pop     bx
        ; client area via the app
        mov     ax, si
        call    win_client
        shl     si, 1
        call    word [app_draw+si]
        popa
        ret

; win_hit: AX,BX = point, DI = window -> ZF=1 if inside the window rectangle
win_hit:
        push    si
        push    bx
        push    ax
        mov     si, di
        push    bx
        call    win_record
        pop     dx
        cmp     ax, [bx+W_X]
        jb      .out
        mov     cx, [bx+W_X]
        add     cx, [bx+W_W]
        cmp     ax, cx
        jae     .out
        cmp     dx, [bx+W_Y]
        jb      .out
        mov     cx, [bx+W_Y]
        add     cx, [bx+W_H]
        cmp     dx, cx
        jae     .out
        xor     cx, cx                          ; ZF = 1
        jmp     .done
.out:   or      cx, 1                           ; ZF = 0
.done:  pop     ax
        pop     bx
        pop     si
        ret

; =============================================================================
; input dispatch
; =============================================================================
; gui_click: AX,BX = screen point (left button pressed)
gui_click:
        pusha
        cmp     byte [menu_open], 0
        je      .no_menu
        call    menu_hit
        mov     byte [menu_open], 0
        cmp     cx, -1
        je      .close_menu
        call    menu_action
        jmp     .done
.close_menu:
        call    gui_redraw
        jmp     .done
.no_menu:
        cmp     bx, [L_task_y]
        jb      .not_taskbar
        ; Start button
        mov     cx, [L_start_w]
        add     cx, 2
        cmp     ax, cx
        jae     .task_buttons
        mov     byte [menu_open], 1
        mov     word [menu_sel], -1
        call    gui_redraw
        jmp     .done
.task_buttons:
        mov     di, [L_start_w]
        add     di, 8
        xor     si, si
.tb:    call    win_record
        cmp     word [bx+W_OPEN], 0
        je      .tb_next
        cmp     ax, di
        jb      .tb_next
        mov     cx, di
        add     cx, [L_taskbtn_w]
        cmp     ax, cx
        jae     .tb_skip
        mov     ax, si
        call    win_raise
        call    gui_redraw
        jmp     .done
.tb_skip:
        add     di, [L_taskbtn_w]
        add     di, 3
.tb_next:
        inc     si
        cmp     si, MAX_WIN
        jb      .tb
        jmp     .done
.not_taskbar:
        mov     si, [z_count]
.wloop: dec     si
        js      .no_window
        movzx   di, byte [z_order+si]
        call    win_hit
        jne     .wloop
        ; inside window DI: bring to front first
        mov     cx, [z_count]
        dec     cx
        cmp     si, cx
        je      .on_top
        push    ax
        mov     ax, di
        call    win_raise
        call    gui_redraw
        pop     ax
.on_top:
        mov     si, di
        call    win_record
        ; title bar?
        mov     cx, [bx+W_Y]
        add     cx, 3
        cmp     bx, cx                          ; (BX register is the record!)
        ; recompute with proper registers
        mov     dx, [mouse_y]
        cmp     dx, cx
        jb      .done                           ; on the top border
        add     cx, [L_title_h]
        cmp     dx, cx
        jae     .client
        ; close button?
        mov     cx, [bx+W_X]
        add     cx, [bx+W_W]
        sub     cx, 21
        cmp     ax, cx
        jb      .drag_start
        mov     ax, si
        call    win_close
        jmp     .done
.drag_start:
        mov     [drag_win], si
        mov     cx, [bx+W_X]
        mov     [drag_x], cx
        sub     ax, cx
        mov     [drag_dx], ax
        mov     cx, [bx+W_Y]
        mov     [drag_y], cx
        sub     dx, cx
        mov     [drag_dy], dx
        call    cursor_hide
        call    drag_outline
        call    cursor_show
        jmp     .done
.client:
        mov     ax, si
        call    win_client
        mov     ax, [mouse_x]
        sub     ax, [cl_x]
        js      .done
        cmp     ax, [cl_w]
        jae     .done
        mov     bx, [mouse_y]
        sub     bx, [cl_y]
        js      .done
        cmp     bx, [cl_h]
        jae     .done
        shl     si, 1
        call    word [app_click+si]
        jmp     .done
.no_window:
        call    icon_hit
        cmp     cx, -1
        je      .desktop
        mov     [sel_icon], cx
        mov     ax, cx
        inc     ax                              ; click id 1..5
        call    check_double_click
        jc      .open_icon
        call    gui_redraw
        jmp     .done
.open_icon:
        mov     ax, [sel_icon]
        call    icon_action
        jmp     .done
.desktop:
        mov     word [sel_icon], -1
        call    gui_redraw
.done:  popa
        ret

; check_double_click: AX = target id -> CF=1 if this completes a double click
check_double_click:
        push    eax
        push    ebx
        push    ecx
        mov     bx, ax
        call    bios_ticks
        mov     ecx, eax
        sub     ecx, [last_click_ticks]
        cmp     ecx, DBLCLICK_TICKS
        ja      .single
        cmp     bx, [last_click_id]
        jne     .single
        mov     word [last_click_id], 0
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
.single:
        mov     [last_click_ticks], eax
        mov     [last_click_id], bx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

; icon_action: AX = icon index
icon_action:
        cmp     ax, 4
        jb      .win
        mov     byte [gui_quit], 1
        ret
.win:   call    win_open
        ret

; gui_release: left button released
gui_release:
        pusha
        cmp     word [drag_win], -1
        je      .done
        call    cursor_hide
        call    drag_outline                    ; erase
        mov     si, [drag_win]
        call    win_record
        mov     ax, [drag_x]
        mov     [bx+W_X], ax
        mov     ax, [drag_y]
        mov     [bx+W_Y], ax
        mov     word [drag_win], -1
        call    gui_redraw
.done:  popa
        ret

; drag_outline: XOR the outline of the dragged window at drag_x/drag_y
drag_outline:
        pusha
        mov     si, [drag_win]
        call    win_record
        mov     ax, [drag_x]
        push    bx
        mov     cx, [bx+W_W]
        mov     dx, [bx+W_H]
        mov     bx, [drag_y]
        call    gfx_xor_rect
        pop     bx
        popa
        ret

; drag_update: the mouse moved while dragging (cursor already hidden)
drag_update:
        pusha
        mov     si, [drag_win]
        call    win_record
        mov     ax, [mouse_x]
        sub     ax, [drag_dx]
        jns     .x1
        xor     ax, ax
.x1:    mov     cx, [scr_w]
        sub     cx, [bx+W_W]
        cmp     ax, cx
        jle     .x2
        mov     ax, cx
.x2:    mov     dx, [mouse_y]
        sub     dx, [drag_dy]
        jns     .y1
        xor     dx, dx
.y1:    mov     cx, [L_task_y]
        sub     cx, [bx+W_H]
        jns     .y2
        xor     cx, cx
.y2:    cmp     dx, cx
        jle     .y3
        mov     dx, cx
.y3:    cmp     ax, [drag_x]
        jne     .changed
        cmp     dx, [drag_y]
        je      .done
.changed:
        call    drag_outline                    ; erase old
        mov     [drag_x], ax
        mov     [drag_y], dx
        call    drag_outline                    ; draw new
.done:  popa
        ret

; -----------------------------------------------------------------------------
; gui_key: AL = ASCII, AH = scan code
; -----------------------------------------------------------------------------
gui_key:
        pusha
        cmp     al, 27
        jne     .not_esc
        ; Ctrl+Esc toggles the Start menu
        push    ax
        mov     ah, 0x02
        int     0x16
        test    al, 0x04
        pop     ax
        jnz     .toggle_menu
        cmp     byte [menu_open], 0
        je      .close_active
        mov     byte [menu_open], 0
        call    gui_redraw
        jmp     .done
.close_active:
        call    gui_active
        cmp     ax, -1
        je      .done
        call    win_close
        jmp     .done
.toggle_menu:
        xor     byte [menu_open], 1
        mov     word [menu_sel], -1
        cmp     byte [menu_open], 0
        je      .redraw
        mov     word [menu_sel], 0
.redraw:
        call    gui_redraw
        jmp     .done
.not_esc:
        cmp     byte [menu_open], 0
        je      .no_menu
        cmp     ah, 0x48                        ; up
        je      .menu_up
        cmp     ah, 0x50                        ; down
        je      .menu_down
        cmp     al, 13
        jne     .done
        mov     cx, [menu_sel]
        mov     byte [menu_open], 0
        cmp     cx, -1
        je      .redraw
        call    menu_action
        jmp     .done
.menu_up:
        mov     cx, [menu_sel]
        dec     cx
        jns     .menu_set
        mov     cx, NUM_MENU - 1
        jmp     .menu_set
.menu_down:
        mov     cx, [menu_sel]
        inc     cx
        cmp     cx, NUM_MENU
        jb      .menu_set
        xor     cx, cx
.menu_set:
        mov     [menu_sel], cx
        call    cursor_hide
        call    draw_menu
        call    cursor_show
        jmp     .done
.no_menu:
        cmp     al, 9                           ; Tab: cycle windows
        je      .cycle
        cmp     ah, 0xA5                        ; Alt+Tab (enhanced keyboards)
        je      .cycle
        cmp     ah, 0x3B                        ; F1: About
        jne     .to_app
        mov     ax, WIN_ABOUT
        call    win_open
        jmp     .done
.cycle: cmp     word [z_count], 2
        jb      .done
        movzx   ax, byte [z_order]
        call    win_raise
        call    gui_redraw
        jmp     .done
.to_app:
        push    ax
        call    gui_active
        mov     si, ax
        pop     ax
        cmp     si, -1
        je      .done
        shl     si, 1
        call    word [app_key+si]
.done:  popa
        ret

; -----------------------------------------------------------------------------
; fmt_dec: EAX (signed) -> decimal string at DS:DI, NUL-terminated.  CX = length
; -----------------------------------------------------------------------------
fmt_dec:
        push    eax
        push    ebx
        push    edx
        push    di
        push    di
        test    eax, eax
        jns     .positive
        neg     eax
        mov     byte [di], '-'
        inc     di
.positive:
        mov     ebx, 10
        xor     cx, cx
.div:   xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .div
.emit:  pop     dx
        add     dl, '0'
        mov     [di], dl
        inc     di
        loop    .emit
        mov     byte [di], 0
        pop     ax
        mov     cx, di
        sub     cx, ax
        pop     di
        pop     edx
        pop     ebx
        pop     eax
        ret

; =============================================================================
section .data
gui_vmode:      db 0
gui_nomouse:    db 0
gui_quit:       db 0
menu_open:      db 0
cursor_visible: db 0
prev_buttons:   db 0
cur_buttons:    db 0
clock_minute:   db 0xFF
menu_sel:       dw -1
sel_icon:       dw -1
drag_win:       dw -1
z_count:        dw 0
mouse_x:        dw 0
mouse_y:        dw 0
L_title_h:      dw 20
L_task_h:       dw 28
L_task_y:       dw 452
L_btn_h:        dw 22
L_item_h:       dw 22
L_start_w:      dw 54
L_taskbtn_w:    dw 110
L_tray_w:       dw 64
L_icon_step:    dw 72
L_menu_w:       dw 160
win_titles:     dw fm_title, np_title, str_calculator, str_about
app_open:       dw fm_open, np_open, calc_open, about_open
app_draw:       dw fm_draw, np_draw, calc_draw, about_draw
app_click:      dw fm_click, np_click, calc_click, about_click
app_key:        dw fm_key, np_key, calc_key, about_key
str_start:      db "Start", 0
str_band:       db "Ember", 0
str_my_computer: db "My Computer", 0
str_notepad:    db "Notepad", 0
str_calculator: db "Calculator", 0
str_about:      db "About Ember", 0
str_about_short: db "About", 0
str_dos_prompt: db "DOS Prompt", 0
str_exit_dos:   db "Exit to DOS", 0
str_shutdown:   db "Shut Down...", 0
msg_gui_no_mem: db "Not enough memory free for the graphical shell.", 13, 10, 0
msg_no_gfx:     db "No supported graphics mode (VESA 8-bit or VGA 320x200).", 13, 10, 0
msg_log_mode:   db "GUI screen (w<<16|h)     ", 0
msg_log_mouse:  db "mouse fail step (0=ok)  ", 0
msg_log_mouse_bios: db "mouse via BIOS assist   ", 0
section .bss
windows:        resb MAX_WIN * W_SIZE
z_order:        resb MAX_WIN
cursor_save:    resb CURSOR_W * CURSOR_H
cursor_sx:      resw 1
cursor_sy:      resw 1
drag_x:         resw 1
drag_y:         resw 1
drag_dx:        resw 1
drag_dy:        resw 1
draw_win:       resw 1
cl_x:           resw 1
cl_y:           resw 1
cl_w:           resw 1
cl_h:           resw 1
last_click_ticks: resd 1
last_click_id:  resw 1
clock_str:      resb 8
section .text
