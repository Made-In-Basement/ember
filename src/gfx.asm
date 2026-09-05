; =============================================================================
;  gfx.asm - 256-colour graphics primitives (VESA 640x480 or VGA 320x200),
;            BIOS font text, palette (mouse driver: mouse.asm)
; -----------------------------------------------------------------------------
;  All drawing goes through a banked window at A000:0000.  In mode 13h the
;  whole screen fits in one bank, so the same code works for both modes.
;
;  Register conventions: AX = x, BX = y, CX = width, DX = height.
;  Colours come from [pen] (fills / lines) and [text_fg] / [text_bg] (text).
;  Every primitive preserves all registers except where noted.
; =============================================================================

VBE_MODE        equ 0x0101              ; 640x480x256
VIDEO_SEG       equ 0xA000

; Windows-95 style palette indices
C_BLACK         equ 0
C_NAVY          equ 1
C_GREEN         equ 2
C_TEAL          equ 3
C_MAROON        equ 4
C_PURPLE        equ 5
C_OLIVE         equ 6
C_SILVER        equ 7
C_GRAY          equ 8
C_BLUE          equ 9
C_LIME          equ 10
C_CYAN          equ 11
C_RED           equ 12
C_MAGENTA       equ 13
C_YELLOW        equ 14
C_WHITE         equ 15
C_LTGRAY        equ 16                  ; extra: light grey for 3D highlights
C_DESKTOP       equ 3

; -----------------------------------------------------------------------------
; gfx_init: enter graphics mode.  AL = requested mode:
;   0 = auto (800x600, then 640x480, then 320x200)   1 = 320x200
;   2 = 640x480   3 = 800x600   4 = 1024x768   (each falls back downwards)
; -----------------------------------------------------------------------------
gfx_init:
        pusha
        push    es
        mov     byte [gfx_ok], 0
        cmp     al, 5                           ; 5 = the widest mode offered
        jne     .not_wide
        call    try_wide_mode
        jnc     .mode_set
        mov     cx, 0x0103                      ; nothing wide: 800x600
        call    try_vbe_mode
        jnc     .mode_set
        jmp     .try_640
.not_wide:
        cmp     al, 1
        je      .low_res
        cmp     al, 4
        jne     .not_1024
        mov     cx, 0x0105                      ; 1024x768x256
        call    try_vbe_mode
        jnc     .mode_set
.not_1024:
        cmp     al, 2
        je      .try_640
        mov     cx, 0x0103                      ; 800x600x256
        call    try_vbe_mode
        jnc     .mode_set
.try_640:
        mov     cx, VBE_MODE                    ; 640x480x256
        call    try_vbe_mode
        jnc     .mode_set
.low_res:
        mov     ax, 0x0013
        int     0x10
        mov     word [scr_w], 320
        mov     word [scr_h], 200
        mov     word [scr_pitch], 320
        mov     byte [font_h], 8
        mov     byte [gfx_mode], 0
        mov     word [bank_mult], 1
.mode_set:
        mov     word [cur_bank], 0xFFFF
        xor     ax, ax
        call    set_bank
        call    gfx_load_font
        call    gfx_set_palette
        mov     byte [gfx_ok], 1
        pop     es
        popa
        clc
        ret

; -----------------------------------------------------------------------------
; try_vbe_mode: CX = VBE mode number.  Queries it, checks it is a banked
;   8-bit mode at A000h, sets it and records the geometry.  CF=1 on failure.
; -----------------------------------------------------------------------------
try_vbe_mode:
        pusha
        push    es
        mov     ax, ds
        mov     es, ax
        mov     di, vbe_info
        mov     ax, 0x4F01
        int     0x10
        cmp     ax, 0x004F
        jne     .fail
        mov     ax, [vbe_info]                  ; mode attributes
        test    al, 0x01                        ; supported by this card
        jz      .fail
        cmp     byte [vbe_info+25], 8           ; bits per pixel
        jne     .fail
        cmp     word [vbe_info+8], VIDEO_SEG    ; window A segment
        jne     .fail
        cmp     word [vbe_info+18], 0           ; X resolution known?
        je      .fail
        mov     ax, [vbe_info+4]                ; window granularity in KB
        or      ax, ax
        jz      .fail
        cmp     ax, 64
        ja      .fail
        mov     bx, ax
        mov     ax, 64
        xor     dx, dx
        div     bx                              ; AX = 64 / granularity
        mov     [bank_mult], ax
        mov     ax, 0x4F02
        mov     bx, cx
        int     0x10
        cmp     ax, 0x004F
        jne     .fail
        mov     ax, [vbe_info+18]
        mov     [scr_w], ax
        mov     ax, [vbe_info+20]
        mov     [scr_h], ax
        mov     ax, [vbe_info+16]               ; bytes per scan line (row stride)
        or      ax, ax
        jnz     .have_pitch
        mov     ax, [scr_w]
.have_pitch:
        mov     [scr_pitch], ax
        mov     byte [font_h], 16
        mov     byte [gfx_mode], 1
        pop     es
        popa
        clc
        ret
.fail:  pop     es
        popa
        stc
        ret

; -----------------------------------------------------------------------------
; try_wide_mode: take the widest mode the card offers whose shape matches a
;   widescreen panel.  A 4:3 mode on such a panel is stretched to fill it,
;   which is what made the splash look wrong.  CF=0 with the mode set.
;
;   The card's list of modes usually lives inside the block it just filled
;   in, so the list is copied out before any mode is asked about: the reply
;   would otherwise land on top of the list being walked.
; -----------------------------------------------------------------------------
WIDE_MAX        equ 96

try_wide_mode:
        pusha
        push    es
        push    ds
        pop     es
        mov     di, vbe_info
        mov     dword [di], "VBE2"
        mov     ax, 0x4F00
        int     0x10
        cmp     ax, 0x004F
        jne     .none
        cmp     dword [vbe_info], "VESA"
        jne     .none

        ; ---- copy the list of mode numbers somewhere safe ----
        mov     ax, [vbe_info+16]               ; its segment
        mov     fs, ax
        mov     si, [vbe_info+14]               ; and offset
        mov     di, wide_modes
        xor     cx, cx
.copy:  mov     ax, [fs:si]
        cmp     ax, 0xFFFF
        je      .copied
        mov     [di], ax
        add     si, 2
        add     di, 2
        inc     cx
        cmp     cx, WIDE_MAX
        jb      .copy
.copied:
        mov     [wide_count], cx
        or      cx, cx
        jnz     .have_list
        jmp     .none
.have_list:

        ; ---- look at each in turn, keeping the largest widescreen one ----
        mov     word [wide_best], 0
        mov     word [wide_area], 0
        xor     bp, bp
.next:  cmp     bp, [wide_count]
        jae     .chose
        mov     si, bp
        shl     si, 1
        mov     cx, [wide_modes+si]
        push    bp
        push    cx
        mov     ax, 0x4F01
        mov     di, vbe_info
        int     0x10
        pop     cx
        pop     bp
        cmp     ax, 0x004F
        jne     .skip
        test    byte [vbe_info], 0x01           ; is it supported?
        jz      .skip
        cmp     byte [vbe_info+25], 8           ; 256 colours
        jne     .skip
        cmp     word [vbe_info+8], VIDEO_SEG    ; reachable through the window
        jne     .skip
        mov     ax, [vbe_info+18]               ; width
        mov     bx, [vbe_info+20]               ; height
        or      bx, bx
        jz      .skip
        cmp     ax, 1024
        jb      .skip                           ; too small to be worth it
        ; the shape: width * 100 / height, wanted between 155 and 185
        push    cx
        push    dx
        xor     dx, dx
        mov     cx, 100
        mul     cx
        div     bx
        mov     cx, ax                          ; CX = the ratio
        pop     dx
        cmp     cx, 155
        jb      .skip_pop
        cmp     cx, 185
        ja      .skip_pop
        pop     cx
        ; keep it if it is the widest so far (an area would not fit 16 bits)
        mov     ax, [vbe_info+18]
        cmp     ax, [wide_area]
        jbe     .skip
        mov     [wide_area], ax
        mov     [wide_best], cx
        jmp     .skip
.skip_pop:
        pop     cx
.skip:  inc     bp
        jmp     .next
.chose:
        cmp     word [wide_best], 0
        je      .none
        mov     cx, [wide_best]
        call    try_vbe_mode
        jc      .none
        pop     es
        popa
        clc
        ret
.none:  pop     es
        popa
        stc
        ret

; -----------------------------------------------------------------------------
; gfx_done: back to 80x25 text mode
; -----------------------------------------------------------------------------
gfx_done:
        call    set_text_mode
        mov     byte [gfx_ok], 0
        ret

; -----------------------------------------------------------------------------
; gfx_load_font: remember where the BIOS keeps its 8x8 or 8x16 font
; -----------------------------------------------------------------------------
gfx_load_font:
        pusha
        push    es
        push    ds
        mov     bh, 0x06                        ; 8x16 ROM font
        cmp     byte [font_h], 16
        je      .get
        mov     bh, 0x03                        ; 8x8 ROM font
.get:   mov     ax, 0x1130
        int     0x10                            ; ES:BP -> ROM font
        mov     ax, es
        mov     [font_seg], ax                  ; glyphs are read from there
        mov     [font_off], bp
        pop     ds
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; gfx_set_palette: program the 17 colours we use
; -----------------------------------------------------------------------------
gfx_set_palette:
        pusha
        ; The VGA DAC normally takes 6-bit values, but some VESA BIOSes switch
        ; it to 8 bits.  Ask (VBE 4F08h) and scale the values to match.
        mov     byte [dac_shift], 2
        cmp     byte [gfx_mode], 0
        je      .write
        mov     ax, 0x4F08
        mov     bl, 0x00                        ; set DAC width...
        mov     bh, 6                           ; ...to 6 bits if possible
        int     0x10
        cmp     ax, 0x004F
        jne     .write
        cmp     bh, 8                           ; BH = width now in effect
        jne     .write
        mov     byte [dac_shift], 0
.write: mov     si, palette
        mov     cx, 17
        xor     al, al
        mov     dx, 0x3C8
        out     dx, al                          ; start at colour 0
        inc     dx
.next:  lodsb
        push    cx
        mov     cl, [dac_shift]
        shr     al, cl
        out     dx, al
        lodsb
        shr     al, cl
        out     dx, al
        lodsb
        shr     al, cl
        out     dx, al
        pop     cx
        loop    .next
        popa
        ret

palette:
        db   0,  0,  0,   0,  0,128,   0,128,  0,   0,128,128
        db 128,  0,  0, 128,  0,128, 128,128,  0, 192,192,192
        db 128,128,128,   0,  0,255,   0,255,  0,   0,255,255
        db 255,  0,  0, 255,  0,255, 255,255,  0, 255,255,255
        db 223,223,223

; -----------------------------------------------------------------------------
; set_bank: AX = 64 KB bank number
; -----------------------------------------------------------------------------
set_bank:
        cmp     ax, [cur_bank]
        je      .done
        mov     [cur_bank], ax
        cmp     byte [gfx_mode], 0
        je      .done                           ; mode 13h: single bank
        pusha
        mul     word [bank_mult]
        mov     dx, ax
        mov     ax, 0x4F05
        xor     bx, bx
        int     0x10
        popa
.done:  ret

; -----------------------------------------------------------------------------
; linear_addr: AX = x, BX = y -> EAX = y * scr_pitch + x
; -----------------------------------------------------------------------------
linear_addr:
        push    edx
        push    ebx
        movzx   eax, ax
        movzx   ebx, bx
        movzx   edx, word [scr_pitch]
        imul    ebx, edx
        add     eax, ebx
        pop     ebx
        pop     edx
        ret

; -----------------------------------------------------------------------------
; span_fill: fill CX pixels with [pen] starting at linear address EAX
; -----------------------------------------------------------------------------
span_fill:
        pushad
        push    es
        mov     dx, VIDEO_SEG
        mov     es, dx
        mov     bl, [pen]
        mov     di, ax                          ; offset in bank
        shr     eax, 16
        call    set_bank
        mov     edx, 0x10000
        movzx   esi, di
        sub     edx, esi                        ; bytes left in this bank
        movzx   esi, cx
        cmp     esi, edx
        jbe     .single
        ; crosses a bank boundary
        mov     cx, dx
        push    ax
        mov     al, bl
        rep     stosb
        pop     ax
        inc     ax
        call    set_bank
        xor     di, di
        mov     cx, si
        sub     cx, dx
.single:
        mov     al, bl
        rep     stosb
        pop     es
        popad
        ret

; -----------------------------------------------------------------------------
; span_copy: copy CX bytes from DS:SI to linear address EAX
; -----------------------------------------------------------------------------
span_copy:
        pushad
        push    es
        mov     dx, VIDEO_SEG
        mov     es, dx
        mov     di, ax
        shr     eax, 16
        call    set_bank
        mov     edx, 0x10000
        movzx   ebx, di
        sub     edx, ebx                        ; bytes left in this bank
        movzx   ebx, cx
        cmp     ebx, edx
        jbe     .single
        mov     cx, dx
        rep     movsb
        inc     ax
        call    set_bank
        xor     di, di
        mov     cx, bx
        sub     cx, dx
.single:
        rep     movsb
        pop     es
        popad
        ret

; -----------------------------------------------------------------------------
; gfx_fill_rect: AX = x, BX = y, CX = w, DX = h, colour [pen].  Clips to screen
; -----------------------------------------------------------------------------
gfx_fill_rect:
        pushad
        ; clip left/top
        test    ax, ax
        jns     .x_ok
        add     cx, ax
        xor     ax, ax
.x_ok:  test    bx, bx
        jns     .y_ok
        add     dx, bx
        xor     bx, bx
.y_ok:  ; clip right/bottom
        mov     si, ax
        add     si, cx
        cmp     si, [scr_w]
        jbe     .w_ok
        mov     cx, [scr_w]
        sub     cx, ax
.w_ok:  mov     si, bx
        add     si, dx
        cmp     si, [scr_h]
        jbe     .h_ok
        mov     dx, [scr_h]
        sub     dx, bx
.h_ok:  cmp     cx, 0
        jle     .done
        cmp     dx, 0
        jle     .done
        call    linear_addr                     ; EAX = start
        movzx   esi, word [scr_pitch]
.row:   call    span_fill
        add     eax, esi
        dec     dx
        jnz     .row
.done:  popad
        ret

; -----------------------------------------------------------------------------
; gfx_hline / gfx_vline: AX = x, BX = y, CX = length, colour [pen]
; -----------------------------------------------------------------------------
gfx_hline:
        push    dx
        mov     dx, 1
        call    gfx_fill_rect
        pop     dx
        ret

gfx_vline:
        push    cx
        push    dx
        mov     dx, cx
        mov     cx, 1
        call    gfx_fill_rect
        pop     dx
        pop     cx
        ret

; -----------------------------------------------------------------------------
; gfx_rect: outline, AX = x, BX = y, CX = w, DX = h, colour [pen]
; -----------------------------------------------------------------------------
gfx_rect:
        pusha
        call    gfx_hline                       ; top
        push    bx
        add     bx, dx
        dec     bx
        call    gfx_hline                       ; bottom
        pop     bx
        push    cx
        mov     cx, dx
        call    gfx_vline                       ; left
        pop     cx
        add     ax, cx
        dec     ax
        mov     cx, dx
        call    gfx_vline                       ; right
        popa
        ret

; -----------------------------------------------------------------------------
; gfx_bevel: Win95 3D border + face.  AX,BX,CX,DX = rect; [bevel] = 0 raised,
;   1 sunken, 2 raised with a thin (button) edge, 3 flat dark frame
; -----------------------------------------------------------------------------
gfx_bevel:
        pusha
        mov     byte [pen], C_SILVER
        call    gfx_fill_rect                   ; face
        cmp     byte [bevel], 1
        je      .sunken
        cmp     byte [bevel], 3
        je      .flat
        ; raised: outer TL white, inner TL light grey / outer BR black, inner BR grey
        mov     byte [pen], C_WHITE
        call    .top_left
        mov     byte [pen], C_BLACK
        call    .bottom_right
        push    ax
        push    bx
        push    cx
        push    dx
        inc     ax
        inc     bx
        sub     cx, 2
        sub     dx, 2
        mov     byte [pen], C_LTGRAY
        call    .top_left
        mov     byte [pen], C_GRAY
        call    .bottom_right
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        jmp     .done
.sunken:
        mov     byte [pen], C_GRAY
        call    .top_left
        mov     byte [pen], C_WHITE
        call    .bottom_right
        push    ax
        push    bx
        push    cx
        push    dx
        inc     ax
        inc     bx
        sub     cx, 2
        sub     dx, 2
        mov     byte [pen], C_BLACK
        call    .top_left
        mov     byte [pen], C_LTGRAY
        call    .bottom_right
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        jmp     .done
.flat:  mov     byte [pen], C_BLACK
        call    gfx_rect
.done:  popa
        ret
.top_left:
        push    cx
        call    gfx_hline
        mov     cx, dx
        call    gfx_vline
        pop     cx
        ret
.bottom_right:
        push    bx
        add     bx, dx
        dec     bx
        call    gfx_hline
        pop     bx
        push    ax
        push    cx
        add     ax, cx
        dec     ax
        mov     cx, dx
        call    gfx_vline
        pop     cx
        pop     ax
        ret

; -----------------------------------------------------------------------------
; gfx_put_pixel: AX = x, BX = y, CL = colour   /  gfx_get_pixel: -> CL
; -----------------------------------------------------------------------------
gfx_put_pixel:
        pushad
        push    es
        cmp     ax, [scr_w]
        jae     .done
        cmp     bx, [scr_h]
        jae     .done
        call    linear_addr
        mov     di, ax
        shr     eax, 16
        call    set_bank
        mov     ax, VIDEO_SEG
        mov     es, ax
        mov     [es:di], cl
.done:  pop     es
        popad
        ret

gfx_get_pixel:
        push    eax
        push    di
        push    es
        cmp     ax, [scr_w]
        jae     .off
        cmp     bx, [scr_h]
        jae     .off
        call    linear_addr
        mov     di, ax
        shr     eax, 16
        call    set_bank
        mov     ax, VIDEO_SEG
        mov     es, ax
        mov     cl, [es:di]
        jmp     .done
.off:   xor     cl, cl
.done:  pop     es
        pop     di
        pop     eax
        ret

; -----------------------------------------------------------------------------
; gfx_xor_rect: XOR a 1-pixel outline with white (drag feedback)
; -----------------------------------------------------------------------------
gfx_xor_rect:
        pusha
        push    ax
        push    cx
        ; top and bottom edges
        mov     si, cx
.top:   call    .xor_px
        push    bx
        add     bx, dx
        dec     bx
        call    .xor_px
        pop     bx
        inc     ax
        dec     si
        jnz     .top
        pop     cx
        pop     ax
        ; left and right edges (excluding corners)
        mov     si, dx
        sub     si, 2
        jle     .done
        inc     bx
.side:  call    .xor_px
        push    ax
        add     ax, cx
        dec     ax
        call    .xor_px
        pop     ax
        inc     bx
        dec     si
        jnz     .side
.done:  popa
        ret
.xor_px:
        push    cx
        call    gfx_get_pixel
        xor     cl, 0x0F
        call    gfx_put_pixel
        pop     cx
        ret

; -----------------------------------------------------------------------------
; gfx_blit_row: AX = x, BX = y, CX = w, DS:SI = pixels.  Clips horizontally.
; -----------------------------------------------------------------------------
gfx_blit_row:
        pushad
        cmp     bx, [scr_h]
        jae     .done
        test    ax, ax
        jns     .left_ok
        sub     si, ax                          ; skip the off-screen part
        add     cx, ax
        xor     ax, ax
.left_ok:
        mov     dx, ax
        add     dx, cx
        cmp     dx, [scr_w]
        jbe     .right_ok
        mov     cx, [scr_w]
        sub     cx, ax
.right_ok:
        cmp     cx, 0
        jle     .done
        call    linear_addr
        call    span_copy
.done:  popad
        ret

; -----------------------------------------------------------------------------
; gfx_char: draw character CL at AX,BX with [text_fg]/[text_bg]
; -----------------------------------------------------------------------------
gfx_char:
        pushad
        mov     bp, ax                          ; BP = x
        movzx   si, cl
        movzx   dx, byte [font_h]
        imul    si, dx
        add     si, [font_off]                  ; SI -> glyph rows in the ROM
        mov     dh, dl                          ; DH = rows remaining
        mov     fs, [font_seg]
.row:   mov     dl, [fs:si]                     ; glyph bits
        mov     di, char_row
        mov     cx, 8
.bit:   mov     al, [text_bg]
        shl     dl, 1
        jnc     .put
        mov     al, [text_fg]
.put:   mov     [di], al
        inc     di
        loop    .bit
        push    si
        mov     si, char_row
        mov     cx, 8
        mov     ax, bp
        call    gfx_blit_row
        pop     si
        inc     si
        inc     bx
        dec     dh
        jnz     .row
        popad
        ret

; -----------------------------------------------------------------------------
; gfx_text: draw NUL-terminated DS:SI at AX,BX.  Returns AX = x after text
; gfx_text_n: same for exactly CX characters
; -----------------------------------------------------------------------------
gfx_text:
        push    cx
        push    si
.next:  mov     cl, [si]
        or      cl, cl
        jz      .done
        call    gfx_char
        add     ax, 8
        inc     si
        jmp     .next
.done:  pop     si
        pop     cx
        ret

gfx_text_n:
        push    cx
        push    si
        jcxz    .done
.next:  push    cx
        mov     cl, [si]
        call    gfx_char
        pop     cx
        add     ax, 8
        inc     si
        loop    .next
.done:  pop     si
        pop     cx
        ret

; -----------------------------------------------------------------------------
; gfx_text_center: centre DS:SI horizontally in [AX, AX+CX) at row BX
; -----------------------------------------------------------------------------
gfx_text_center:
        pusha
        mov     di, si
        xor     dx, dx
.len:   cmp     byte [di], 0
        je      .got
        inc     di
        inc     dx
        jmp     .len
.got:   shl     dx, 3                           ; text width in pixels
        sub     cx, dx
        sar     cx, 1
        add     ax, cx
        call    gfx_text
        popa
        ret

; -----------------------------------------------------------------------------
; gfx_draw_image: AX = x, BX = y, CX = w, DX = h, DS:SI = ASCII-art pixels
;   ('.' = transparent, 0-9/A-F = palette colour, 'G' = light grey)
; -----------------------------------------------------------------------------
gfx_draw_image:
        pushad
        mov     bp, dx                          ; BP = rows remaining
.row:   push    ax
        push    si
        mov     di, cx                          ; DI = pixels left in this row
.px:    mov     dl, [si]
        cmp     dl, '.'
        je      .skip
        push    cx
        call    art_colour                      ; DL char -> CL colour
        call    gfx_put_pixel
        pop     cx
.skip:  inc     si
        inc     ax
        dec     di
        jnz     .px
        pop     si
        pop     ax
        add     si, cx
        inc     bx
        dec     bp
        jnz     .row
        popad
        ret

; art_colour: DL = character -> CL = palette index
art_colour:
        mov     cl, dl
        sub     cl, '0'
        cmp     cl, 9
        jbe     .done
        sub     cl, 7                           ; 'A'..'G'
.done:  ret

; -----------------------------------------------------------------------------
; data
; -----------------------------------------------------------------------------
section .data
font_seg:       dw 0xF000
font_off:       dw 0xFA6E                       ; the usual ROM font address
gfx_ok:         db 0
gfx_mode:       db 0                            ; 0 = 320x200, 1 = 640x480
font_h:         db 16
scr_w:          dw 640
scr_h:          dw 480
scr_pitch:      dw 640
dac_shift:      db 2
bank_mult:      dw 1
wide_best:      dw 0
wide_area:      dw 0
wide_count:     dw 0
cur_bank:       dw 0xFFFF
pen:            db 0
bevel:          db 0
text_fg:        db 0
text_bg:        db 7
mouse_ok:       db 0
mouse_buttons:  db 0
mouse_dx:       dw 0
mouse_dy:       dw 0
section .bss
vbe_info:       resb 512
wide_modes:     resw WIDE_MAX
char_row:       resb 8
section .text
