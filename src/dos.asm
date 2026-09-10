; =============================================================================
;  dos.asm - the DOS program environment: processes, .COM/.EXE loading, EXEC,
;            INT 21h / 20h / 27h / 29h / 2Fh services, FindFirst/Next
; -----------------------------------------------------------------------------
;  Programs live in MCB-headed blocks of the arena (mem.asm), get a real PSP
;  and environment block, may EXEC other programs (nesting), and terminate
;  back to whoever started them: the shell, or a parent program's INT 21h
;  AH=4Bh call.  Enough of the DOS API is provided for real-mode programs
;  and for 32-bit extenders such as DOS/4GW to start.
; =============================================================================

; Offsets into the register frame built by int21_handler (BP-relative, SS)
%define R_EDI   dword [bp+0]
%define R_ESI   dword [bp+4]
%define R_EBX   dword [bp+16]
%define R_EDX   dword [bp+20]
%define R_ECX   dword [bp+24]
%define R_EAX   dword [bp+28]
%define R_DI    word [bp+0]
%define R_SI    word [bp+4]
%define R_BX    word [bp+16]
%define R_DX    word [bp+20]
%define R_CX    word [bp+24]
%define R_AX    word [bp+28]
%define R_AL    byte [bp+28]
%define R_AH    byte [bp+29]
%define R_BL    byte [bp+16]
%define R_BH    byte [bp+17]
%define R_CL    byte [bp+24]
%define R_CH    byte [bp+25]
%define R_DL    byte [bp+20]
%define R_DH    byte [bp+21]
%define R_ES    word [bp+32]
%define R_DS    word [bp+34]
%define R_FLAGS word [bp+40]

; ---- PSP layout ---------------------------------------------------------------
PSP_TOP         equ 0x02
PSP_INT22       equ 0x0A
PSP_INT23       equ 0x0E
PSP_INT24       equ 0x12
PSP_PARENT      equ 0x16
PSP_JFT         equ 0x18
PSP_ENV         equ 0x2C
PSP_STACK       equ 0x2E
PSP_JFT_SIZE    equ 0x32
PSP_JFT_PTR     equ 0x34
PSP_DISPATCH    equ 0x50
PSP_FCB1        equ 0x5C
PSP_FCB2        equ 0x6C
PSP_TAIL        equ 0x80

; ---- file handles ---------------------------------------------------------------
MAX_HANDLES     equ 16
FIRST_HANDLE    equ 5
HANDLE_SIZE     equ 32
H_START         equ 0                   ; dw first cluster
H_SIZE          equ 2                   ; dd file size
H_POS           equ 6                   ; dd position
H_CUR           equ 10                  ; dw current cluster
H_CUR_INDEX     equ 12                  ; dw index of current cluster in chain
H_USED          equ 14                  ; db in use
H_OWNER         equ 16                  ; dw owning PSP
H_DATE          equ 18                  ; dw FAT date
H_TIME          equ 20                  ; dw FAT time
H_DIR_LBA       equ 22                  ; dd sector holding the directory entry
H_DIR_OFF       equ 26                  ; dw offset of the entry in that sector
H_FLAGS         equ 28                  ; db
HF_WRITE        equ 0x01                ; opened for writing
HF_DIRTY        equ 0x02                ; entry needs rewriting on close

; ---- process context stack ----------------------------------------------------
MAX_PROC        equ 8
PC_SS           equ 0
PC_SP           equ 2
PC_PSP          equ 4                   ; PSP of the parent
PC_TYPE         equ 6                   ; 0 = started by the shell, 1 = INT 21h EXEC
PC_DTA_OFF      equ 8
PC_DTA_SEG      equ 10
PC_SIZE         equ 12

; ---- MZ header ---------------------------------------------------------------
MZ_LAST_BYTES   equ 2
MZ_PAGES        equ 4
MZ_RELOCS       equ 6
MZ_HDR_PARAS    equ 8
MZ_MINALLOC     equ 10
MZ_MAXALLOC     equ 12
MZ_SS           equ 14
MZ_SP           equ 16
MZ_IP           equ 20
MZ_CS           equ 22
MZ_RELOC_OFF    equ 24

; =============================================================================
; install_interrupts: our vectors, plus harmless stubs for the DOS-era
;   interrupts programs probe or hook
; =============================================================================
install_interrupts:
        push    es
        pusha
        xor     ax, ax
        mov     es, ax
        cli
        ; INT 20h-2Fh, 33h, 67h all point at an IRET first
        mov     cx, 16
        mov     bx, 0x20*4
.stubs: mov     word [es:bx], int_iret
        mov     [es:bx+2], cs
        add     bx, 4
        loop    .stubs
        mov     word [es:0x33*4], int_iret
        mov     [es:0x33*4+2], cs
        mov     word [es:0x67*4], int67_handler
        mov     [es:0x67*4+2], cs
        mov     word [es:0x20*4], int20_handler
        mov     [es:0x20*4+2], cs
        mov     word [es:0x21*4], int21_handler
        mov     [es:0x21*4+2], cs
        mov     word [es:0x22*4], int22_handler
        mov     [es:0x22*4+2], cs
        mov     word [es:0x24*4], int24_handler
        mov     [es:0x24*4+2], cs
        mov     word [es:0x27*4], int20_handler
        mov     [es:0x27*4+2], cs
        mov     word [es:0x29*4], int29_handler
        mov     [es:0x29*4+2], cs
        mov     word [es:0x2F*4], int2f_handler
        mov     [es:0x2F*4+2], cs
        sti
        popa
        pop     es
        ret

int_iret:
        iret

; INT 24h critical error handler: fail the operation
int24_handler:
        mov     al, 3
        iret

; INT 67h: no expanded memory manager
int67_handler:
        mov     ah, 0x84                        ; function not supported
        iret

; INT 22h: a program jumped to its termination address
int22_handler:
        mov     byte [cs:exit_code], 0
        jmp     proc_exit

; INT 29h: fast console output (AL = char)
int29_handler:
        push    ax
        push    bx
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        pop     bx
        pop     ax
        iret

; INT 2Fh: multiplex.  Only the "is X installed?" probes get an answer.
;   Drivers that are modules (XMS.MOD) hook this vector ahead of us and
;   answer for themselves.
int2f_handler:
        cmp     ax, 0x1687                      ; DPMI installation check
        jne     .not_dpmi
        ; Only a program about to enter protected mode asks this.  Once it is
        ; there it owns the interrupt table, and the speaker bridge's I/O
        ; breakpoints - which live in the processor and stay armed across the
        ; switch - would arrive at its handler instead of ours.  DOS/4GW
        ; reports that as a fatal debug exception and stops.  So the bridge
        ; stands down here; the shell starts it again at the next prompt.
        push    ds
        push    cs
        pop     ds
        call    spk_bridge_stop
        pop     ds
        mov     ax, 0x0001                      ; non-zero: no DPMI host
        iret
.not_dpmi:
        cmp     ax, 0x1680                      ; release time slice
        jne     .done
        mov     al, 0x80                        ; not supported
.done:  iret

; INT 20h / INT 27h: terminate
int20_handler:
        cmp     word [cs:cur_psp], 0
        je      .none
        mov     byte [cs:exit_code], 0
        jmp     proc_exit
.none:  iret

; =============================================================================
; INT 21h dispatcher
; =============================================================================
STK_FRAME       equ 0x2000 - 48                 ; top 48 bytes of the block
STK_IVT         equ 0                           ; bottom 1 KB: the saved vectors

int21_handler:
        push    ds
        push    es
        pushad
        mov     [cs:caller_ss], ss
        mov     [cs:caller_sp], sp
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        ; copy the register frame (pushes + IRET frame, 42 bytes) into our
        ; own memory, then run on the kernel stack, as DOS does: programs with
        ; small stacks must not have to hold our pushes and the BIOS's
        mov     si, [caller_sp]
        mov     es, [stk_seg]
        mov     di, STK_FRAME
        mov     cx, 21
        push    ds
        mov     ds, [caller_ss]
        rep     movsw
        pop     ds
        mov     es, ax
        cli
        mov     ss, [stk_seg]
        mov     sp, STK_FRAME                   ; pushes go below the frame
        sti
        cld
        mov     bp, STK_FRAME                   ; R_* now address the copy (SS:BP)
        and     R_FLAGS, 0xFFFE                 ; CF = 0 unless a handler sets it
        mov     byte [indos_flag], 1
        mov     al, R_AH
        cmp     byte [trace_flag], 0
        je      .no_trace
        call    trace_byte                      ; AL = function number
.no_trace:
        mov     si, int21_table
.find:  mov     ah, [si]
        cmp     ah, 0xFF
        je      .unsupported
        cmp     ah, al
        je      .call
        add     si, 3
        jmp     .find
.call:  call    word [si+1]
        jmp     int21_done
.unsupported:
        mov     R_AX, 1                         ; invalid function
        or      R_FLAGS, 1
int21_done:
        mov     byte [cs:path_api], 0
        mov     byte [cs:indos_flag], 0
        ; copy the (possibly modified) frame back and return on the caller's stack
        mov     ax, cs
        mov     ds, ax
        cmp     byte [trace_flag], 0
        je      .no_trace2
        mov     bp, STK_FRAME
        mov     al, '@'
        call    log_putc
        mov     al, [bp+39]                     ; return CS:IP high bytes
        call    trace_byte
        mov     al, [bp+38]
        call    trace_byte
        mov     al, [bp+37]
        call    trace_byte
        mov     al, [bp+36]
        call    trace_byte
        mov     al, [bp+41]                     ; flags
        call    trace_byte
        mov     al, [bp+40]
        call    trace_byte
        mov     al, 10
        call    log_putc
.no_trace2:
        mov     es, [caller_ss]
        mov     di, [caller_sp]
        mov     si, STK_FRAME
        mov     cx, 21
        push    ds
        mov     ds, [stk_seg]
        rep     movsw
        pop     ds
        cli
        mov     ss, [caller_ss]
        mov     sp, [caller_sp]
        sti
int21_direct:                                   ; SS:SP -> frame on the caller's stack
        mov     byte [cs:indos_flag], 0
        popad
        pop     es
        pop     ds
        iret

int21_table:
        db 0x00
        dw f4c
        db 0x01
        dw f01
        db 0x02
        dw f02
        db 0x06
        dw f06
        db 0x07
        dw f07
        db 0x08
        dw f07
        db 0x09
        dw f09
        db 0x0A
        dw f0a
        db 0x0B
        dw f0b
        db 0x0C
        dw f0c
        db 0x0D
        dw f_ok
        db 0x0E
        dw f0e
        db 0x19
        dw f19
        db 0x1A
        dw f1a
        db 0x25
        dw f25
        db 0x26
        dw f26
        db 0x2A
        dw f2a
        db 0x2B
        dw f_ok_al0
        db 0x2C
        dw f2c
        db 0x2D
        dw f_ok_al0
        db 0x2E
        dw f_ok
        db 0x2F
        dw f2f
        db 0x30
        dw f30
        db 0x33
        dw f33
        db 0x34
        dw f34
        db 0x35
        dw f35
        db 0x36
        dw f36
        db 0x37
        dw f37
        db 0x38
        dw f38
        db 0x3B
        dw f3b
        db 0x39
        dw f39
        db 0x3A
        dw f3a
        db 0x3C
        dw f3c
        db 0x3D
        dw f3d
        db 0x3E
        dw f3e
        db 0x3F
        dw f3f
        db 0x40
        dw f40
        db 0x41
        dw f41
        db 0x42
        dw f42
        db 0x43
        dw f43
        db 0x44
        dw f44
        db 0x45
        dw f45
        db 0x46
        dw f46
        db 0x47
        dw f47
        db 0x48
        dw f48
        db 0x49
        dw f49
        db 0x4A
        dw f4a
        db 0x4B
        dw f4b
        db 0x4C
        dw f4c
        db 0x4D
        dw f4d
        db 0x4E
        dw f4e
        db 0x4F
        dw f4f
        db 0x50
        dw f50
        db 0x51
        dw f62
        db 0x52
        dw f52
        db 0x54
        dw f_ok_al0
        db 0x56
        dw f56
        db 0xF3
        dw ff3
        db 0x57
        dw f57
        db 0x58
        dw f58
        db 0x5A
        dw f_denied
        db 0x5B
        dw f_denied
        db 0x5C
        dw f_ok
        db 0x60
        dw f60
        db 0x62
        dw f62
        db 0x67
        dw f_ok
        db 0x68
        dw f68
        db 0x6C
        dw f6c
        db 0x71
        dw f71
        db 0xF0
        dw fF0
        db 0xF1
        dw fF1
        db 0xF2
        dw fF2
        db 0xF4
        dw fF4
        db 0xFF

; ---- helpers ----------------------------------------------------------------
set_cf:
        or      R_FLAGS, 1
        ret

; trace_byte: append AL as two hex digits and a space to the boot log
trace_byte:
        push    ax
        push    cx
        mov     ah, al
        shr     al, 4
        call    .digit
        mov     al, ah
        and     al, 0x0F
        call    .digit
        mov     al, ' '
        call    log_putc
        pop     cx
        pop     ax
        ret
.digit: add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    log_putc
        ret

; trace_mark: AL = marker character (when tracing)
trace_mark:
        cmp     byte [trace_flag], 0
        je      .done
        call    log_putc
        mov     al, ' '
        call    log_putc
.done:  ret

f_ok:   ret
f_ok_al0:
        mov     R_AL, 0
        ret
f_denied:
        mov     R_AX, 5                         ; access denied (read-only FS)
        jmp     set_cf
f_not_found:
        mov     R_AX, 2
        jmp     set_cf

; dos_getkey: like getkey, but extended keys return 0 then the scan code
dos_getkey:
        mov     al, [pending_key]
        or      al, al
        jz      .read
        mov     byte [pending_key], 0
        ret
.read:  call    getkey
        or      al, al
        jnz     .done
        mov     [pending_key], ah
.done:  ret

; copy_caller_path: DS:DX (caller) -> path_buf, drive prefix kept
copy_caller_path:
        mov     byte [path_api], 1              ; spaces are part of a name here
        push    ds
        mov     ds, R_DS
        mov     si, R_DX
        mov     di, path_buf
        mov     cx, 78
.copy:  lodsb
        stosb
        or      al, al
        jz      .done
        loop    .copy
        mov     byte [es:di], 0
.done:  pop     ds
        ret

; ---- 01h: read char with echo ----------------------------------------------
f01:    call    dos_getkey
        call    putc
        mov     R_AL, al
        ret

; ---- 02h: write char ---------------------------------------------------------
f02:    mov     al, R_DL
        call    putc
        mov     R_AL, al
        ret

; ---- 06h: direct console I/O ------------------------------------------------
f06:    mov     al, R_DL
        cmp     al, 0xFF
        je      .input
        call    putc
        mov     R_AL, al
        ret
.input: cmp     byte [pending_key], 0
        jne     .have
        call    kbhit
        jz      .none
.have:  call    dos_getkey
        mov     R_AL, al
        and     R_FLAGS, 0xFFBF                 ; ZF = 0
        ret
.none:  mov     R_AL, 0
        or      R_FLAGS, 0x0040                 ; ZF = 1
        ret

; ---- 07h/08h: read char, no echo --------------------------------------------
f07:    call    dos_getkey
        mov     R_AL, al
        ret

; ---- 09h: print $-terminated string at DS:DX --------------------------------
f09:    mov     ds, R_DS
        mov     si, R_DX
.next:  lodsb
        cmp     al, '$'
        je      .done
        call    putc
        jmp     .next
.done:  mov     ax, cs
        mov     ds, ax
        mov     R_AL, '$'
        ret

; ---- 0Ah: buffered keyboard input -------------------------------------------
f0a:    mov     es, R_DS
        mov     di, R_DX
        movzx   cx, byte [es:di]                ; buffer capacity
        jcxz    .done
        dec     cx                              ; room for the CR
        cmp     cx, 127
        jbe     .ok
        mov     cx, 127
.ok:    push    di
        mov     di, line_buf
        call    readline                        ; AX = length
        pop     di
        mov     [es:di+1], al
        mov     cx, ax
        mov     si, line_buf
        add     di, 2
        rep     movsb
        mov     byte [es:di], 13
        mov     al, 13
        call    putc
.done:  mov     ax, cs
        mov     es, ax
        ret

; ---- 0Bh: keyboard status ----------------------------------------------------
f0b:    mov     R_AL, 0xFF
        cmp     byte [pending_key], 0
        jne     .done
        call    kbhit
        jnz     .done
        mov     R_AL, 0
.done:  ret

; ---- 0Ch: flush keyboard, then perform function AL --------------------------
f0c:    mov     byte [pending_key], 0
        call    kb_flush
        mov     al, R_AL
        mov     R_AH, al
        cmp     al, 0x01
        je      f01
        cmp     al, 0x06
        je      f06
        cmp     al, 0x07
        je      f07
        cmp     al, 0x08
        je      f07
        cmp     al, 0x0A
        je      f0a
        ret

; ---- 0Eh: select disk -> AL = number of drives ------------------------------
f0e:    movzx   ax, byte [drive_number]
        inc     ax
        mov     R_AL, al
        ret

; ---- 19h: current drive ------------------------------------------------------
f19:    mov     al, [drive_number]
        mov     R_AL, al
        ret

; ---- 1Ah: set DTA  /  2Fh: get DTA -------------------------------------------
f1a:    mov     ax, R_DX
        mov     [dta_off], ax
        mov     ax, R_DS
        mov     [dta_seg], ax
        ret
f2f:    mov     ax, [dta_off]
        mov     R_BX, ax
        mov     ax, [dta_seg]
        mov     R_ES, ax
        ret

; ---- 25h: set interrupt vector AL = DS:DX -----------------------------------
f25:    movzx   bx, R_AL
        shl     bx, 2
        xor     ax, ax
        mov     es, ax
        mov     ax, R_DX
        mov     cx, R_DS
        cli
        mov     [es:bx], ax
        mov     [es:bx+2], cx
        sti
        mov     ax, cs
        mov     es, ax
        ret

; ---- 26h: create PSP at segment DX (copy of the current one) ----------------
f26:    push    ds
        mov     es, R_DX
        mov     ds, [cur_psp]
        xor     si, si
        xor     di, di
        mov     cx, 128
        rep     movsw
        pop     ds
        mov     ax, cs
        mov     es, ax
        ret

; ---- 2Ah: get date -> CX = year, DH = month, DL = day, AL = weekday ---------
f2a:    call    rtc_read_date                   ; AX=year, DH=month, DL=day
        mov     R_CX, ax
        mov     R_DH, dh
        mov     R_DL, dl
        call    day_of_week
        mov     R_AL, al
        ret

; ---- 2Ch: get time -> CH = hour, CL = min, DH = sec, DL = 1/100 -------------
f2c:    call    rtc_read_time                   ; CH=hour, CL=min, DH=sec
        mov     R_CH, ch
        mov     R_CL, cl
        mov     R_DH, dh
        mov     R_DL, 0
        ret

; ---- 30h: get version -> AL = 5, AH = 0 (reported for compatibility) --------
f30:    mov     R_AX, 0x0005
        mov     R_BX, 0xFF00                    ; OEM: Microsoft-style
        mov     R_CX, 0
        ret

; ---- 33h: Ctrl-Break checking / version ---------------------------------------
f33:    mov     al, R_AL
        cmp     al, 0
        jne     .not_get
        mov     R_DL, 0
        ret
.not_get:
        cmp     al, 6
        jne     .done
        mov     R_BX, 0x0005                    ; BL = major, BH = minor
        mov     R_DX, 0
.done:  ret

; ---- 34h: InDOS flag address -> ES:BX -----------------------------------------
f34:    mov     R_BX, indos_flag
        mov     R_ES, cs
        ret

; ---- 35h: get interrupt vector AL -> ES:BX ----------------------------------
f35:    movzx   bx, R_AL
        shl     bx, 2
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:bx]
        mov     R_BX, ax
        mov     ax, [es:bx+2]
        mov     R_ES, ax
        mov     ax, cs
        mov     es, ax
        ret

; ---- 36h: disk free space -> AX = spc, BX = free clusters, CX = 512, DX = total
f36:    cmp     byte [fs_ok], 0
        je      .bad
        movzx   ax, byte [fs_spc]
        mov     R_AX, ax
        mov     R_CX, 512
        mov     ax, [fs_total_clusters]
        mov     R_DX, ax
        call    fs_free_clusters
        mov     R_BX, ax
        ret
.bad:   mov     R_AX, 0xFFFF
        ret

; ---- 37h: switch character ---------------------------------------------------
f37:    mov     R_DL, '/'
        mov     R_AL, 0
        ret

; ---- 38h: country information (US defaults) ----------------------------------
f38:    mov     es, R_DS
        mov     di, R_DX
        mov     cx, 34
        xor     al, al
        rep     stosb
        mov     di, R_DX
        mov     word [es:di+2], '$'             ; currency symbol
        mov     word [es:di+7], ','             ; thousands separator
        mov     word [es:di+9], '.'             ; decimal separator
        mov     word [es:di+11], '-'            ; date separator
        mov     word [es:di+13], ':'            ; time separator
        mov     byte [es:di+17], 2              ; digits after decimal
        mov     ax, cs
        mov     es, ax
        mov     R_BX, 1                         ; country code
        ret

; ---- 3Bh: change directory (DS:DX) --------------------------------------------
f3b:    call    copy_caller_path
        mov     si, path_buf
        call    strip_drive
        call    change_dir
        jc      .bad
        ret
.bad:   mov     R_AX, 3                         ; path not found
        jmp     set_cf

; ---- 3Dh: open file (DS:DX = ASCIIZ path) -> AX = handle --------------------
f3d:    call    copy_caller_path
        call    open_path_buf
        jc      .done
        mov     al, R_AL
        and     al, 0x03
        jz      .done                           ; opened for reading only
        mov     bx, R_AX
        call    handle_lookup
        jc      .done
        or      byte [bx+H_FLAGS], HF_WRITE
.done:  ret

; ---- 6Ch: extended open (DS:SI path, DX action) ----------------------------
f6c:    push    ds
        mov     ds, R_DS
        mov     si, R_SI
        mov     di, path_buf
        mov     cx, 78
.copy:  lodsb
        stosb
        or      al, al
        jz      .copied
        loop    .copy
        mov     byte [es:di], 0
.copied:
        pop     ds
        mov     ax, R_DX
        mov     [ext_action], al
        test    al, 0x03                        ; may we open an existing file?
        jz      .try_create
        call    open_path_buf
        jc      .try_create                     ; it is not there
        test    byte [ext_action], 0x02         ; truncate it?
        jz      .opened
        mov     bx, R_AX
        call    handle_lookup
        jc      .opened
        mov     dword [bx+H_SIZE], 0
        mov     dword [bx+H_POS], 0
        mov     ax, [bx+H_START]
        or      ax, ax
        jz      .cut
        call    fat_free_chain
        mov     word [bx+H_START], 0
        mov     word [bx+H_CUR], 0
        mov     word [bx+H_CUR_INDEX], 0
.cut:   or      byte [bx+H_FLAGS], HF_DIRTY
        call    fs_commit_handle
        mov     R_CX, 3                         ; file truncated
        jmp     .access
.opened:
        mov     R_CX, 1                         ; file opened
        jmp     .access
.try_create:
        test    byte [ext_action], 0x10         ; may we create it?
        jz      f_not_found
        mov     si, path_buf
        call    fs_create
        jc      .fail
        call    open_path_buf
        jc      .done
        mov     R_CX, 2                         ; file created
.access:
        mov     bx, R_AX
        call    handle_lookup
        jc      .done
        mov     al, R_BL
        and     al, 0x03
        jz      .done
        or      byte [bx+H_FLAGS], HF_WRITE
.done:  ret
.fail:  mov     R_AX, ax
        jmp     set_cf

; open_path_buf: open the file named in path_buf -> R_AX = handle
open_path_buf:
        cmp     byte [fs_ok], 0
        je      f_not_found
        mov     si, path_buf
        call    strip_drive
        jc      f_not_found
        call    resolve_path
        jc      f_not_found
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     f_denied
        ; find a free slot
        mov     bx, handles
        xor     cx, cx
.slot:  cmp     byte [bx+H_USED], 0
        je      .free
        add     bx, HANDLE_SIZE
        inc     cx
        cmp     cx, MAX_HANDLES
        jb      .slot
        mov     R_AX, 4                         ; too many open files
        jmp     set_cf
.free:  mov     byte [bx+H_USED], 1
        mov     ax, [found_cluster]
        mov     [bx+H_START], ax
        mov     [bx+H_CUR], ax
        mov     word [bx+H_CUR_INDEX], 0
        mov     eax, [found_size]
        mov     [bx+H_SIZE], eax
        mov     dword [bx+H_POS], 0
        mov     ax, [cur_psp]
        mov     [bx+H_OWNER], ax
        mov     ax, [found_date]
        mov     [bx+H_DATE], ax
        mov     ax, [found_time]
        mov     [bx+H_TIME], ax
        mov     ax, [found_dir_off]             ; where its directory entry is,
        mov     [bx+H_DIR_OFF], ax              ;  so writes can update it
        mov     eax, [found_dir_lba]
        mov     [bx+H_DIR_LBA], eax
        mov     byte [bx+H_FLAGS], 0
        add     cx, FIRST_HANDLE
        mov     R_AX, cx
        clc
        ret

; strip_drive: skip a leading "C:" in DS:SI.  CF=1 if it names another drive
strip_drive:
        push    ax
        cmp     byte [si+1], ':'
        jne     .ok
        mov     al, [si]
        call    upcase
        sub     al, 'A'
        cmp     al, [drive_number]
        jne     .other
        add     si, 2
.ok:    pop     ax
        clc
        ret
.other: pop     ax
        stc
        ret

; handle_lookup: BX = handle -> BX = record, CF=1 if invalid
handle_lookup:
        sub     bx, FIRST_HANDLE
        jb      .bad
        cmp     bx, MAX_HANDLES
        jae     .bad
        push    ax
        mov     ax, HANDLE_SIZE
        mul     bx
        mov     bx, ax
        pop     ax
        add     bx, handles
        cmp     byte [bx+H_USED], 0
        je      .bad
        clc
        ret
.bad:   stc
        ret

; handles_close_owner: AX = PSP -> close every handle it owns
handles_close_owner:
        push    bx
        push    cx
        mov     bx, handles
        mov     cx, MAX_HANDLES
.next:  cmp     [bx+H_OWNER], ax
        jne     .skip
        cmp     byte [bx+H_USED], 0
        je      .skip
        push    ax
        call    fs_commit_handle                ; do not lose a file it wrote
        pop     ax
        mov     byte [bx+H_USED], 0
.skip:  add     bx, HANDLE_SIZE
        loop    .next
        pop     cx
        pop     bx
        ret

; ---- 3Eh: close handle -------------------------------------------------------
f3e:    mov     bx, R_BX
        cmp     bx, FIRST_HANDLE
        jb      .std                            ; closing stdin/out is fine
        call    handle_lookup
        jc      .bad
        call    fs_commit_handle                ; the directory entry, if dirty
        mov     byte [bx+H_USED], 0
.std:   ret
.bad:   mov     R_AX, 6
        jmp     set_cf

; ---- 68h: commit a file to disk --------------------------------------------
f68:    mov     bx, R_BX
        cmp     bx, FIRST_HANDLE
        jb      .done
        call    handle_lookup
        jc      .done
        call    fs_commit_handle
.done:  ret

; ---- 3Fh: read from handle (BX) CX bytes into DS:DX -> AX = bytes read ------
f3f:    mov     bx, R_BX
        test    bx, bx
        jnz     .not_stdin
        ; --- stdin: read a line (echoed), returns text + CR LF ---
        mov     cx, R_CX
        cmp     cx, 2
        jb      .zero
        sub     cx, 2
        cmp     cx, 127
        jbe     .cap_ok
        mov     cx, 127
.cap_ok:
        mov     di, line_buf
        call    readline
        call    crlf
        mov     es, R_DS
        mov     di, R_DX
        mov     cx, ax
        mov     si, line_buf
        rep     movsb
        mov     byte [es:di], 13
        mov     byte [es:di+1], 10
        add     ax, 2
        mov     R_AX, ax
        mov     ax, cs
        mov     es, ax
        ret
.zero:  mov     R_AX, 0
        ret
.not_stdin:
        cmp     bx, FIRST_HANDLE
        jb      .zero                           ; stdout/aux/prn: nothing to read
        call    handle_lookup
        jc      .bad
        mov     ecx, dword R_ECX
        and     ecx, 0xFFFF
        mov     [hr_count], ecx
        mov     dword [hr_total], 0
        mov     es, R_DS
        mov     di, R_DX
.loop:
        cmp     dword [hr_count], 0
        je      .finish
        mov     eax, [bx+H_SIZE]
        sub     eax, [bx+H_POS]
        jbe     .finish                         ; at EOF
        movzx   ecx, byte [fs_spc]
        shl     ecx, 9
        mov     eax, [bx+H_POS]
        xor     edx, edx
        div     ecx                             ; EAX = cluster index, EDX = offset
        mov     [hr_off], edx
        cmp     ax, [bx+H_CUR_INDEX]
        jae     .walk
        push    ax
        mov     ax, [bx+H_START]
        mov     [bx+H_CUR], ax
        mov     word [bx+H_CUR_INDEX], 0
        pop     ax
.walk:  cmp     ax, [bx+H_CUR_INDEX]
        je      .located
        push    ax
        mov     ax, [bx+H_CUR]
        call    next_cluster
        jc      .walk_fail
        mov     [bx+H_CUR], ax
        inc     word [bx+H_CUR_INDEX]
        pop     ax
        jmp     .walk
.walk_fail:
        pop     ax
        jmp     .finish
.located:
        mov     ax, [bx+H_CUR]
        call    cluster_to_lba
        mov     edx, [hr_off]
        shr     edx, 9
        add     eax, edx
        push    es
        push    bx
        push    di
        mov     bx, ds
        mov     es, bx
        mov     bx, sector_buf
        call    read_sector
        pop     di
        pop     bx
        pop     es
        jc      .finish
        mov     ecx, [hr_off]
        and     ecx, 511
        mov     si, sector_buf
        add     si, cx
        mov     eax, 512
        sub     eax, ecx                        ; bytes left in sector
        cmp     eax, [hr_count]
        jbe     .n1
        mov     eax, [hr_count]
.n1:    mov     edx, [bx+H_SIZE]
        sub     edx, [bx+H_POS]
        cmp     eax, edx
        jbe     .n2
        mov     eax, edx
.n2:    mov     cx, ax
        rep     movsb
        add     [bx+H_POS], eax
        add     [hr_total], eax
        sub     [hr_count], eax
        jmp     .loop
.finish:
        mov     eax, [hr_total]
        mov     dword R_EAX, eax
        mov     ax, cs
        mov     es, ax
        ret
.bad:   mov     R_AX, 6
        jmp     set_cf

; ---- 40h: write CX bytes from DS:DX to handle BX ----------------------------
f40:    mov     bx, R_BX
        cmp     bx, 1
        je      .console
        cmp     bx, 2
        je      .console
        cmp     bx, 3                           ; aux / prn: swallow
        je      .swallow
        cmp     bx, 4
        je      .swallow
        cmp     bx, FIRST_HANDLE
        jb      .bad
        call    handle_lookup
        jc      .bad
        test    byte [bx+H_FLAGS], HF_WRITE
        jz      f_denied
        mov     ax, R_DS
        mov     [w_src_seg], ax
        mov     ax, R_DX
        mov     [w_src_off], ax
        mov     ecx, dword R_ECX
        and     ecx, 0xFFFF
        mov     [w_count], ecx
        mov     dword [w_total], 0
        or      ecx, ecx
        jz      .truncate
.loop:
        cmp     dword [w_count], 0
        je      .finish
        ; which cluster of the file, and where inside it
        movzx   ecx, byte [fs_spc]
        shl     ecx, 9                          ; ECX = bytes per cluster
        mov     eax, [bx+H_POS]
        xor     edx, edx
        div     ecx
        mov     [w_off], edx                    ; offset within the cluster
        call    fs_chain_cluster                ; AX = cluster (extends the file)
        jc      .disk_full
        call    cluster_to_lba
        mov     edx, [w_off]
        shr     edx, 9
        add     eax, edx                        ; EAX = sector to write
        mov     [w_lba], eax
        mov     ecx, [w_off]
        and     ecx, 511                        ; offset within that sector
        mov     [w_sec_off], ecx
        mov     eax, 512
        sub     eax, ecx
        cmp     eax, [w_count]
        jbe     .have_n
        mov     eax, [w_count]
.have_n:
        mov     [w_n], eax                      ; bytes to put in this sector
        cmp     eax, 512
        je      .no_read                        ; a whole sector: no need to read
        push    bx
        push    es
        push    ds
        pop     es
        mov     eax, [w_lba]
        mov     bx, sector_buf
        call    read_sector                     ; keep the bytes we do not write
        pop     es
        pop     bx
        jnc     .no_read
        push    di                              ; a fresh sector: start clean
        push    cx
        push    es
        push    ds
        pop     es
        mov     di, sector_buf
        mov     cx, 512
        xor     al, al
        rep     stosb
        pop     es
        pop     cx
        pop     di
.no_read:
        ; copy the bytes from the caller into the sector image
        push    ds
        push    si
        push    di
        push    cx
        push    es
        push    cs
        pop     es
        mov     di, sector_buf
        add     di, [cs:w_sec_off]
        mov     cx, [cs:w_n]
        mov     si, [cs:w_src_off]
        mov     ds, [cs:w_src_seg]
        rep     movsb
        pop     es
        pop     cx
        pop     di
        pop     si
        pop     ds
        push    bx
        push    es
        push    ds
        pop     es
        mov     eax, [w_lba]
        mov     bx, sector_buf
        call    write_sector
        pop     es
        pop     bx
        jc      .write_error
        ; advance everything
        mov     eax, [w_n]
        add     [bx+H_POS], eax
        add     [w_total], eax
        sub     [w_count], eax
        mov     ecx, [bx+H_POS]
        cmp     ecx, [bx+H_SIZE]
        jbe     .size_ok
        mov     [bx+H_SIZE], ecx                ; the file just grew
.size_ok:
        or      byte [bx+H_FLAGS], HF_DIRTY
        mov     ax, [w_n]
        add     [w_src_off], ax
        jnc     .loop
        mov     ax, [w_src_seg]                 ; the buffer crossed 64 KB
        add     ax, 0x1000
        mov     [w_src_seg], ax
        jmp     .loop
.truncate:
        ; a zero-length write cuts the file off at the current position
        mov     eax, [bx+H_POS]
        cmp     eax, [bx+H_SIZE]
        jae     .no_cut
        mov     [bx+H_SIZE], eax
        or      byte [bx+H_FLAGS], HF_DIRTY
.no_cut:
        call    fs_commit_handle
        mov     R_AX, 0
        ret
.finish:
        call    fs_commit_handle                ; keep the entry in step
        mov     eax, [w_total]
        mov     dword R_EAX, eax
        ret
.disk_full:
.write_error:
        call    fs_commit_handle
        mov     eax, [w_total]
        mov     dword R_EAX, eax
        ret
.console:
        mov     ds, R_DS
        mov     si, R_DX
        mov     cx, R_CX
        call    puts_n
        mov     ax, cs
        mov     ds, ax
.swallow:
        mov     ax, R_CX
        mov     R_AX, ax
        ret
.bad:   mov     R_AX, 6
        jmp     set_cf

; ---- 3Ch: create or truncate a file, CX = attributes -> AX = handle --------
f3c:    call    copy_caller_path
        mov     si, path_buf
        call    fs_create
        jc      .fail
        call    open_path_buf                   ; the file exists now
        jc      .done
        mov     bx, R_AX
        call    handle_lookup
        jc      .done
        or      byte [bx+H_FLAGS], HF_WRITE | HF_DIRTY
.done:  ret
.fail:  mov     R_AX, ax
        jmp     set_cf

; ---- 41h: delete a file ----------------------------------------------------
f41:    call    copy_caller_path
        mov     si, path_buf
        call    fs_delete
        jc      .fail
        mov     R_AX, 0
        ret
.fail:  mov     R_AX, ax
        jmp     set_cf

; ---- F3h: the long name of the entry just found (DS:DX = 80-byte buffer) ----
;   AX = its length, 0 when the file has only its 8.3 name.  Ember's own,
;   so a program that wants long names can ask; DOS programs never do.
ff3:    push    es
        push    ds
        mov     es, R_DS
        mov     di, R_DX
        mov     si, lfn_name
        xor     cx, cx
.copy:  lodsb
        stosb
        or      al, al
        jz      .done
        inc     cx
        cmp     cx, LFN_MAX
        jb      .copy
        xor     al, al
        stosb
.done:  pop     ds
        pop     es
        mov     R_AX, cx
        ret

; ---- 39h: make directory (DS:DX = path) -------------------------------------
f39:    call    copy_caller_path
        mov     si, path_buf
        call    fs_mkdir
        jc      .fail
        mov     R_AX, 0
        ret
.fail:  mov     R_AX, ax
        jmp     set_cf

; ---- 3Ah: remove directory (DS:DX = path) ------------------------------------
f3a:    call    copy_caller_path
        mov     si, path_buf
        call    fs_rmdir
        jc      .fail
        mov     R_AX, 0
        ret
.fail:  mov     R_AX, ax
        jmp     set_cf

; ---- 56h: rename (DS:DX = old path, ES:DI = new path) ------------------------
f56:    call    copy_caller_path
        call    copy_caller_path2
        mov     si, path_buf
        mov     di, copy_dst
        call    fs_rename
        jc      .fail
        mov     R_AX, 0
        ret
.fail:  mov     R_AX, ax
        jmp     set_cf

; copy_caller_path2: ES:DI (caller) -> copy_dst
copy_caller_path2:
        push    ds
        mov     ds, R_ES
        mov     si, R_DI
        mov     di, copy_dst
        mov     cx, 78
.copy:  lodsb
        stosb
        or      al, al
        jz      .done
        loop    .copy
        mov     byte [es:di], 0
.done:  pop     ds
        ret

; ---- 42h: seek (AL = origin, CX:DX = offset) -> DX:AX = new position --------
f42:    mov     bx, R_BX
        call    handle_lookup
        jc      .bad
        mov     eax, dword R_ECX
        shl     eax, 16
        mov     ax, R_DX                        ; EAX = signed offset
        mov     cl, R_AL
        cmp     cl, 1
        je      .cur
        cmp     cl, 2
        je      .end
        jmp     .apply
.cur:   add     eax, [bx+H_POS]
        jmp     .apply
.end:   add     eax, [bx+H_SIZE]
.apply: test    eax, eax
        jns     .store
        xor     eax, eax
.store: mov     [bx+H_POS], eax
        mov     R_AX, ax
        shr     eax, 16
        mov     R_DX, ax
        ret
.bad:   mov     R_AX, 6
        jmp     set_cf

; ---- 43h: get/set file attributes ---------------------------------------------
f43:    call    copy_caller_path
        mov     si, path_buf
        call    strip_drive
        jc      f_not_found
        call    resolve_path
        jc      f_not_found
        cmp     R_AL, 0
        jne     .set
        movzx   ax, byte [found_attr]
        mov     R_CX, ax
        ret
.set:   ret                                     ; pretend it worked

; ---- 44h: IOCTL ----------------------------------------------------------------
f44:    mov     al, R_AL
        cmp     al, 0
        je      .info
        cmp     al, 1
        je      .ok
        cmp     al, 6
        je      .in_status
        cmp     al, 7
        je      .out_status
        cmp     al, 8
        je      .removable
        cmp     al, 9
        je      .remote
        cmp     al, 0x0A
        je      .remote
        mov     R_AX, 1
        jmp     set_cf
.info:  mov     bx, R_BX
        cmp     bx, 3
        jb      .console
        cmp     bx, FIRST_HANDLE
        jb      .device
        call    handle_lookup
        jc      .badhandle
        movzx   ax, byte [drive_number]
        or      ax, 0x0040                      ; disk file, not written
        mov     R_DX, ax
        mov     R_AX, ax
        ret
.console:
        mov     R_DX, 0x80D3                    ; character device: CON
        mov     R_AX, 0x80D3
        ret
.device:
        mov     R_DX, 0x80C0                    ; character device: AUX/PRN
        mov     R_AX, 0x80C0
        ret
.in_status:
        mov     R_AL, 0xFF
        mov     bx, R_BX
        cmp     bx, 0
        jne     .ok
        call    kbhit
        jnz     .ok
        mov     R_AL, 0
.ok:    ret
.out_status:
        mov     R_AL, 0xFF
        ret
.removable:
        mov     R_AX, 1                         ; fixed disk
        ret
.remote:
        mov     R_DX, 0
        ret
.badhandle:
        mov     R_AX, 6
        jmp     set_cf

; ---- 45h: duplicate handle ----------------------------------------------------
f45:    mov     bx, R_BX
        cmp     bx, FIRST_HANDLE
        jb      .std
        call    handle_lookup
        jc      .bad
        mov     si, bx
        mov     bx, handles
        xor     cx, cx
.slot:  cmp     byte [bx+H_USED], 0
        je      .free
        add     bx, HANDLE_SIZE
        inc     cx
        cmp     cx, MAX_HANDLES
        jb      .slot
        mov     R_AX, 4
        jmp     set_cf
.free:  mov     di, bx
        push    cx
        mov     cx, HANDLE_SIZE
        rep     movsb
        pop     cx
        add     cx, FIRST_HANDLE
        mov     R_AX, cx
        ret
.std:   mov     R_AX, bx                        ; console handles: same one
        ret
.bad:   mov     R_AX, 6
        jmp     set_cf

; ---- 46h: force duplicate (BX -> CX) -----------------------------------------
f46:    mov     bx, R_CX
        cmp     bx, FIRST_HANDLE
        jb      .ok
        call    handle_lookup
        jc      .ok
        mov     byte [bx+H_USED], 0             ; close the target first
.ok:    ret                                     ; (the copy itself is not tracked)

; ---- 47h: get current directory (DL = drive, DS:SI = 64-byte buffer) --------
f47:    mov     es, R_DS
        mov     di, R_SI
        mov     si, cur_path
        inc     si                              ; without the leading backslash
.copy:  lodsb
        stosb
        or      al, al
        jnz     .copy
        mov     ax, cs
        mov     es, ax
        mov     R_AX, 0x0100
        ret

; ---- 48h: allocate BX paragraphs -> AX = segment ------------------------------
f48:    mov     bx, R_BX
        call    mem_alloc
        jc      .fail
        mov     R_AX, ax
        ret
.fail:  mov     R_BX, bx
        mov     R_AX, 8                         ; insufficient memory
        jmp     set_cf

; ---- 49h: free block ES -------------------------------------------------------
f49:    mov     es, R_ES
        call    mem_free
        mov     ax, cs
        mov     es, ax
        jc      .bad
        ret
.bad:   mov     R_AX, 9                         ; invalid block
        jmp     set_cf

; ---- 4Ah: resize block ES to BX paragraphs ------------------------------------
f4a:    mov     es, R_ES
        mov     bx, R_BX
        call    mem_resize
        mov     ax, cs
        mov     es, ax
        jc      .fail
        ret
.fail:  mov     R_BX, bx
        mov     R_AX, 8
        jmp     set_cf

; ---- 4Bh: EXEC (AL = 0: load and run DS:DX, ES:BX = parameter block) --------
f4b:    cmp     R_AL, 0
        jne     .unsupported
        call    copy_caller_path
        ; command tail from the parameter block: dword pointer at +2
        push    ds
        mov     ds, R_ES
        mov     si, R_BX
        lds     si, [si+2]                      ; DS:SI -> length byte
        mov     di, exec_tail
        movzx   cx, byte [si]
        inc     si
        cmp     cx, 126
        jbe     .len_ok
        mov     cx, 126
.len_ok:
        mov     [es:exec_tail_len], cl
        rep     movsb
        pop     ds
        mov     byte [exec_type], 1
        mov     si, path_buf
        call    exec_program                    ; returns only on failure
        mov     R_AX, ax                        ; error code
        jmp     set_cf
.unsupported:
        mov     R_AX, 1
        jmp     set_cf

; ---- 4Ch: terminate with return code ----------------------------------------
f4c:    mov     al, R_AL
        mov     [exit_code], al
        cmp     word [cur_psp], 0
        je      .none
        jmp     proc_exit
.none:  ret

; ---- 4Dh: get exit code of the last child -------------------------------------
f4d:    mov     al, [exit_code]
        mov     R_AL, al
        mov     R_AH, 0
        mov     byte [exit_code], 0
        ret

; ---- 50h: set PSP  /  51h, 62h: get PSP -----------------------------------------
f50:    mov     ax, R_BX
        mov     [cur_psp], ax
        ret
f62:    mov     ax, [cur_psp]
        mov     R_BX, ax
        ret

; ---- 52h: list of lists -> ES:BX (first MCB segment at ES:[BX-2]) ------------
f52:    mov     ax, [arena_first]
        mov     [sysvars-2], ax
        mov     R_BX, sysvars
        mov     R_ES, cs
        ret

; ---- 57h: get/set file date and time ------------------------------------------
f57:    mov     bx, R_BX
        call    handle_lookup
        jc      .bad
        cmp     R_AL, 0
        jne     .set
        mov     ax, [bx+H_TIME]
        mov     R_CX, ax
        mov     ax, [bx+H_DATE]
        mov     R_DX, ax
        ret
.set:   ret
.bad:   mov     R_AX, 6
        jmp     set_cf

; ---- 58h: allocation strategy ------------------------------------------------
f58:    mov     R_AX, 0                         ; first fit, low memory
        ret

; ---- 60h: canonicalise path (DS:SI -> ES:DI) ----------------------------------
f60:    push    ds
        mov     ds, R_DS
        mov     si, R_SI
        mov     es, R_ES
        mov     di, R_DI
        cmp     byte [si+1], ':'
        je      .copy
        mov     al, [cs:drive_letter]
        stosb
        mov     al, ':'
        stosb
        cmp     byte [si], '\'
        je      .copy
        push    si
        mov     si, cur_path
        push    ds
        mov     ax, cs
        mov     ds, ax
.cwd:   lodsb
        or      al, al
        jz      .cwd_done
        stosb
        jmp     .cwd
.cwd_done:
        pop     ds
        pop     si
        cmp     byte [es:di-1], '\'
        je      .copy
        mov     al, '\'
        stosb
.copy:  lodsb
        call    upcase
        stosb
        or      al, al
        jnz     .copy
        pop     ds
        mov     ax, cs
        mov     es, ax
        ret

; ---- 71h: long filename functions: not supported ------------------------------
f71:    mov     R_AX, 0x7100
        jmp     set_cf

; =============================================================================
; FindFirst / FindNext (4Eh / 4Fh) through the DTA
;   DTA[0..1] directory cluster, [2..3] next entry index, [4] attribute mask,
;   [5..15] 11-byte pattern, [16] 'N' signature; results at [15h..]
; =============================================================================
f4e:    call    copy_caller_path
        mov     si, path_buf
        call    strip_drive
        jc      f_not_found
        ; split directory part and the final component
        mov     word [ff_last_sep], 0           ; a stale one would cut the path
        mov     ax, [cur_dir_cluster]
        mov     [ff_dir], ax
        mov     di, si
.scan:  cmp     byte [di], 0
        je      .split_done
        cmp     byte [di], '\'
        je      .sep
        cmp     byte [di], '/'
        jne     .adv
.sep:   mov     [ff_last_sep], di
.adv:   inc     di
        jmp     .scan
.split_done:
        cmp     word [ff_last_sep], 0
        je      .pattern_only
        mov     di, [ff_last_sep]
        mov     byte [di], 0                    ; cut the directory part
        push    di
        cmp     di, si
        jne     .resolve_dir
        mov     word [ff_dir], 0                ; "\NAME": root
        jmp     .dir_ok
.resolve_dir:
        call    resolve_path
        jc      .dir_bad
        test    byte [found_attr], ATTR_DIRECTORY
        jz      .dir_bad
        mov     ax, [found_cluster]
        mov     [ff_dir], ax
.dir_ok:
        pop     si
        inc     si                              ; SI -> final component
        jmp     .pattern
.dir_bad:
        pop     di
        jmp     f_not_found
.pattern_only:
.pattern:
        mov     di, ff_pattern
        call    to_fat_name                     ; wildcards become '?'
        ; fill the DTA search state
        push    ds
        push    es
        mov     es, [dta_seg]
        mov     di, [dta_off]
        mov     ax, [ff_dir]
        mov     [es:di], ax
        mov     word [es:di+2], 0
        mov     al, R_CL
        mov     [es:di+4], al
        push    di
        add     di, 5
        mov     si, ff_pattern
        mov     cx, 11
        rep     movsb
        pop     di
        mov     byte [es:di+16], 'N'
        pop     es
        pop     ds
        call    find_next_match
        jc      .none
        ret
.none:  jmp     f_not_found

f4f:    push    es
        mov     es, [dta_seg]
        mov     di, [dta_off]
        cmp     byte [es:di+16], 'N'
        pop     es
        jne     .bad
        call    find_next_match
        jc      .none
        ret
.none:  mov     R_AX, 18                        ; no more files
        jmp     set_cf
.bad:   mov     R_AX, 18
        jmp     set_cf

; find_next_match: continue the search described in the DTA.  CF=1 if none
find_next_match:
        push    es
        mov     es, [dta_seg]
        mov     di, [dta_off]
        mov     ax, [es:di]                     ; directory cluster
        mov     cx, [es:di+2]                   ; entries already examined
        mov     dl, [es:di+4]                   ; attribute mask
        push    di
        lea     si, [di+5]
        mov     di, ff_pattern
        push    cx
        mov     cx, 11
.getpat:
        mov     bl, [es:si]
        mov     [di], bl
        inc     si
        inc     di
        loop    .getpat
        pop     cx
        pop     di
        ; walk the directory to the saved position
        mov     ax, [es:di]
        push    es
        mov     bx, ds
        mov     es, bx
        call    dir_open
.skip:  jcxz    .search
        call    dir_next
        jc      .end_pop
        dec     cx
        jmp     .skip
        call    lfn_begin
.search:
        call    dir_next
        jc      .end_pop
        inc     word [ff_index]                 ; (scratch; real index stored below)
        cmp     byte [si], 0xE5
        je      .search_reset
        mov     al, [si+11]
        cmp     al, ATTR_LFN
        je      .search_long
        ; attribute rule: hidden/system/directory/volume need the mask bit
        mov     ah, al
        and     ah, 0x1E
        not     dl
        test    ah, dl
        not     dl
        jnz     .search
        ; name match against the pattern
        push    si
        mov     di, ff_pattern
        mov     cx, 11
.cmp:   mov     al, [di]
        cmp     al, '?'
        je      .cmp_next
        cmp     al, [si]
        jne     .no_match
.cmp_next:
        inc     si
        inc     di
        loop    .cmp
        pop     si
        ; ---- match: fill the DTA ----
        call    lfn_apply                       ; its long name, if it has one
        pop     es                              ; ES = DTA segment
        mov     di, [dta_off]
        mov     ax, [di_index]                  ; entries consumed so far in this sector...
        ; recompute the absolute index: entries examined = saved + walked
        mov     ax, [es:di+2]
        add     ax, [ff_index]
        mov     [es:di+2], ax
        mov     al, [si+11]
        mov     [es:di+0x15], al
        mov     ax, [si+22]
        mov     [es:di+0x16], ax
        mov     ax, [si+24]
        mov     [es:di+0x18], ax
        mov     eax, [si+28]
        mov     [es:di+0x1A], eax
        push    di
        add     di, 0x1E
        push    es
        mov     ax, ds
        mov     es, ax
        push    di
        mov     di, name_str
        call    fat_name_to_str                 ; DS:name_str = "NAME.EXT"
        pop     di
        pop     es
        mov     si, name_str
        mov     cx, 13
.copyname:
        lodsb
        stosb
        or      al, al
        jz      .name_done
        loop    .copyname
.name_done:
        pop     di
        mov     word [ff_index], 0
        pop     es
        clc
        ret
.no_match:
        pop     si
.search_reset:
        call    lfn_begin                       ; that set was not ours
        jmp     .search
.search_long:
        call    lfn_collect
        jmp     .search
.end_pop:
        pop     es
        pop     es
        mov     word [ff_index], 0
        stc
        ret

; =============================================================================
; Program loading and execution
; =============================================================================
; exec_program: DS:SI = path (NUL-terminated, may include directories).
;   exec_tail / exec_tail_len = command tail bytes.  exec_type = 0 when the
;   shell starts a program (returns via program_return -> RET to the caller),
;   1 when called from INT 21h AH=4Bh (the child returns to the parent's
;   INT 21h frame).  Returns only on failure, with AX = DOS error code.
; -----------------------------------------------------------------------------
exec_program:
        mov     [exec_path_ptr], si
        cmp     byte [proc_depth], MAX_PROC
        jae     .no_memory
        call    strip_drive
        jc      .not_found
        call    resolve_path
        jc      .not_found
        test    byte [found_attr], ATTR_DIRECTORY
        jnz     .not_found
        mov     ax, [found_cluster]
        mov     [exec_cluster], ax
        mov     eax, [found_size]
        mov     [exec_size], eax
        mov     ax, [found_date]
        mov     [exec_date], ax
        ; ---- full path for the environment block ----
        call    build_full_path                 ; -> exec_full
        ; ---- environment block ----
        call    build_environment               ; -> exec_env_seg, CF on failure
        jc      .no_memory
        ; ---- is it an MZ executable? ----
        call    fstream_open
        call    fstream_word
        mov     byte [exec_is_exe], 0
        cmp     ax, 'MZ'
        je      .exe
        cmp     ax, 'ZM'
        je      .exe
        cmp     ax, 'NX'
        jne     .com
        call    fstream_word
        cmp     ax, '32'
        je      .nx32
        call    fstream_open                    ; rewind for load_com
        jmp     .com
.nx32:  ; a 32-bit Ember program: no PSP or environment
        mov     es, [exec_env_seg]
        call    mem_free
        push    cs
        pop     es
        ; how we were called, and where the parent's frame is: its own
        ; INT 21h calls will overwrite caller_ss/caller_sp while it runs
        mov     al, [exec_type]
        mov     [nx_ret_type], al
        mov     ax, [caller_ss]
        mov     [nx_ret_ss], ax
        mov     ax, [caller_sp]
        mov     [nx_ret_sp], ax
        mov     ax, [dta_seg]
        mov     [nx_ret_dta_seg], ax
        mov     ax, [dta_off]
        mov     [nx_ret_dta_off], ax
        call    ivt_save                        ; restored on the way out
        mov     al, MOD_EV_NATIVE               ; a module that put the machine
        call    mod_event                       ;  somewhere else puts it back
        call    spk_bridge_stop                 ; its breakpoints would fire in
        call    run_nx32                        ;  protected mode; back after
        ret
.exe:   mov     byte [exec_is_exe], 1
        call    load_exe
        jc      .load_failed
        jmp     .loaded
.com:   call    load_com
        jc      .load_failed
.loaded:
        ; ---- PSP ----
        call    build_psp
        ; hand the environment to the new process
        mov     es, [exec_env_seg]
        mov     ax, [exec_psp]
        call    mem_set_owner
        mov     es, [exec_psp]
        call    mem_set_owner
        mov     ax, cs
        mov     es, ax
        ; ---- save the parent's context ----
        movzx   bx, byte [proc_depth]
        imul    bx, PC_SIZE
        add     bx, proc_stack
        mov     [bx+PC_SS], ss
        mov     ax, [cur_psp]
        mov     [bx+PC_PSP], ax
        mov     al, [exec_type]
        mov     [bx+PC_TYPE], al
        mov     ax, [dta_off]
        mov     [bx+PC_DTA_OFF], ax
        mov     ax, [dta_seg]
        mov     [bx+PC_DTA_SEG], ax
        cmp     byte [exec_type], 0
        jne     .from_int21
        mov     [bx+PC_SP], sp                  ; RET address of our caller is here
        call    ivt_save
        jmp     .context_saved
.from_int21:
        mov     ax, [caller_ss]                 ; the parent's own stack, where
        mov     [bx+PC_SS], ax                  ; its INT 21h frame still sits
        mov     ax, [caller_sp]
        mov     [bx+PC_SP], ax
.context_saved:
        inc     byte [proc_depth]
        mov     al, 'X'
        call    trace_mark
        mov     ax, [exec_psp]
        mov     [cur_psp], ax
        mov     [dta_seg], ax
        mov     word [dta_off], PSP_TAIL
        mov     byte [pending_key], 0
        ; ---- start it ----
        cli
        mov     ax, [exec_psp]
        mov     ds, ax
        mov     es, ax
        cmp     byte [cs:exec_is_exe], 0
        jne     .start_exe
        mov     ss, ax
        mov     sp, [cs:exec_sp]
        sti
        xor     ax, ax
        xor     bx, bx
        mov     cx, 0x00FF
        xor     dx, dx
        mov     si, 0x0100
        mov     di, sp
        xor     bp, bp
        push    word [cs:exec_psp]
        push    word 0x0100
        retf
.start_exe:
        mov     ss, [cs:exec_ss]
        mov     sp, [cs:exec_sp]
        sti
        xor     ax, ax
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        xor     di, di
        xor     bp, bp
        push    word [cs:exec_cs]
        push    word [cs:exec_ip]
        retf
        ; ---- failures ----
.load_failed:
        push    ax
        mov     es, [exec_env_seg]
        call    mem_free
        cmp     word [exec_psp], 0
        je      .no_block
        mov     es, [exec_psp]
        call    mem_free
.no_block:
        mov     ax, cs
        mov     es, ax
        pop     ax
        stc
        ret
.not_found:
        mov     ax, 2
        stc
        ret
.no_memory:
        mov     ax, 8
        stc
        ret

; build_full_path: exec_path_ptr -> exec_full = "C:\DIR\NAME.EXT" (upper case)
build_full_path:
        pusha
        mov     si, [exec_path_ptr]
        mov     di, exec_full
        mov     al, [drive_letter]
        stosb
        mov     al, ':'
        stosb
        cmp     byte [si], '\'
        je      .abs
        cmp     byte [si], '/'
        je      .abs
        push    si
        mov     si, cur_path
.cwd:   lodsb
        or      al, al
        jz      .cwd_done
        stosb
        jmp     .cwd
.cwd_done:
        pop     si
        cmp     byte [di-1], '\'
        je      .rel
        mov     al, '\'
        stosb
        jmp     .rel
.abs:   inc     si
        mov     al, '\'
        stosb
.rel:   mov     cx, 64
.copy:  lodsb
        call    upcase
        cmp     al, '/'
        jne     .store
        mov     al, '\'
.store: stosb
        or      al, al
        jz      .done
        loop    .copy
        mov     byte [di], 0
.done:  popa
        ret

; blaster_wanted: is there a card for a program to find?  A game reads
;   BLASTER out of its environment and believes it, so the variable is there
;   only while the monitor that answers for the card is loaded.  CF=1: no.
blaster_wanted:
        push    si
        push    bx
        mov     si, name_sb
        call    mod_find_name
        jnc     .yes
        mov     si, name_dpmi                   ; the DPMI host answers for a
        call    mod_find_name                   ;  card too, for its clients
.yes:   pop     bx
        pop     si
        ret

; build_environment: master environment + program path -> exec_env_seg
build_environment:
        pusha
        push    es
        mov     byte [env_blaster_on], 0
        call    blaster_wanted
        jc      .no_card
        mov     byte [env_blaster_on], 1
.no_card:
        ; length: master env (through the double NUL) + 2 + path + NUL
        mov     si, master_env
        xor     cx, cx
.len:   inc     cx
        cmp     byte [si], 0
        jne     .len_more
        cmp     byte [si+1], 0
        je      .len_done
.len_more:
        inc     si
        jmp     .len
.len_done:
        inc     cx                              ; second NUL
        add     cx, 2                           ; count word
        cmp     byte [env_blaster_on], 0
        je      .no_blaster
        add     cx, env_blaster_end - env_blaster
.no_blaster:
        mov     di, exec_full
.plen:  cmp     byte [di], 0
        je      .plen_done
        inc     di
        inc     cx
        jmp     .plen
.plen_done:
        inc     cx                              ; NUL
        add     cx, 15
        shr     cx, 4
        mov     bx, cx
        call    mem_alloc
        jc      .fail
        mov     [exec_env_seg], ax
        mov     es, ax
        xor     di, di
        mov     si, master_env
.copy:  lodsb
        stosb
        or      al, al
        jne     .copy
        cmp     byte [si], 0
        jne     .copy
        cmp     byte [env_blaster_on], 0
        je      .no_blaster2
        push    si
        mov     si, env_blaster
.blaster:
        lodsb
        stosb
        or      al, al
        jne     .blaster
        pop     si
.no_blaster2:
        xor     al, al
        stosb                                   ; second NUL
        mov     ax, 1
        stosw
        mov     si, exec_full
.path:  lodsb
        stosb
        or      al, al
        jnz     .path
        pop     es
        popa
        clc
        ret
.fail:  pop     es
        popa
        stc
        ret

; load_com: allocate the largest block and load the file at PSP:0100
load_com:
        pusha
        push    es
        mov     word [exec_psp], 0
        mov     eax, [exec_size]
        cmp     eax, 0xFE00
        ja      .too_big
        call    mem_largest                     ; BX = paragraphs
        cmp     bx, 32
        jb      .no_memory
        call    mem_alloc
        jc      .no_memory
        mov     [exec_psp], ax
        mov     [exec_block_paras], bx
        ; stack: top of the block, at most 0FFFEh
        mov     ax, bx
        cmp     ax, 0x1000
        jb      .small
        mov     word [exec_sp], 0xFFFE
        jmp     .sp_ok
.small: shl     ax, 4
        sub     ax, 2
        mov     [exec_sp], ax
.sp_ok: ; the file must fit below the stack
        mov     eax, [exec_size]
        add     eax, 0x100 + 0x40
        movzx   ecx, word [exec_sp]
        cmp     eax, ecx
        ja      .no_memory_free
        call    fstream_open
        mov     es, [exec_psp]
        mov     di, 0x0100
        mov     ecx, [exec_size]
        call    fstream_read
        ; word 0 at the top of the stack so a RET ends the program
        mov     di, [exec_sp]
        mov     word [es:di], 0
        pop     es
        popa
        clc
        ret
.no_memory_free:
        mov     es, [exec_psp]
        call    mem_free
        mov     word [exec_psp], 0
.no_memory:
        pop     es
        popa
        mov     ax, 8
        stc
        ret
.too_big:
        pop     es
        popa
        mov     ax, 8
        stc
        ret

; load_exe: parse the MZ header, allocate, load the image, relocate
load_exe:
        pusha
        push    es
        mov     word [exec_psp], 0
        mov     word [exec_hdr_seg], 0
        ; ---- header into a temporary block ----
        call    fstream_open
        push    cs
        pop     es
        mov     di, exe_hdr
        mov     ecx, 32
        call    fstream_read
        mov     bx, [exe_hdr+MZ_HDR_PARAS]
        or      bx, bx
        jz      .bad
        call    mem_alloc
        jc      .no_memory
        mov     [exec_hdr_seg], ax
        call    fstream_open
        mov     es, ax
        xor     di, di
        movzx   ecx, word [exe_hdr+MZ_HDR_PARAS]
        shl     ecx, 4
        call    fstream_read                    ; stream now sits at the image
        ; ---- image size ----
        movzx   eax, word [exe_hdr+MZ_PAGES]
        shl     eax, 9
        movzx   ecx, word [exe_hdr+MZ_LAST_BYTES]
        jecxz   .no_partial
        sub     eax, 512
        add     eax, ecx
.no_partial:
        movzx   ecx, word [exe_hdr+MZ_HDR_PARAS]
        shl     ecx, 4
        sub     eax, ecx                        ; EAX = image bytes
        mov     [exec_image_size], eax
        add     eax, 15
        shr     eax, 4                          ; image paragraphs
        mov     [exec_image_paras], ax
        ; ---- memory: PSP + image + minalloc .. maxalloc ----
        call    mem_largest                     ; BX = largest
        mov     cx, [exec_image_paras]
        add     cx, 16
        add     cx, [exe_hdr+MZ_MINALLOC]
        jc      .no_memory
        cmp     cx, bx
        ja      .no_memory
        mov     dx, [exec_image_paras]
        add     dx, 16
        add     dx, [exe_hdr+MZ_MAXALLOC]
        jc      .want_all
        cmp     dx, bx
        jbe     .have_want
.want_all:
        mov     dx, bx
.have_want:
        mov     bx, dx
        call    mem_alloc
        jc      .no_memory
        mov     [exec_psp], ax
        mov     [exec_block_paras], bx
        ; ---- load the image at PSP+10h:0 ----
        add     ax, 16
        mov     [exec_load_seg], ax
        mov     es, ax
        xor     di, di
        mov     ecx, [exec_image_size]
        call    fstream_read
        ; ---- relocations ----
        mov     cx, [exe_hdr+MZ_RELOCS]
        jcxz    .relocated
        push    ds
        mov     ds, [exec_hdr_seg]
        mov     si, [cs:exe_hdr+MZ_RELOC_OFF]
.reloc: lodsw
        mov     di, ax                          ; offset
        lodsw
        add     ax, [cs:exec_load_seg]          ; segment
        mov     es, ax
        mov     ax, [cs:exec_load_seg]
        add     [es:di], ax
        loop    .reloc
        pop     ds
.relocated:
        ; ---- initial registers ----
        mov     ax, [exe_hdr+MZ_SS]
        add     ax, [exec_load_seg]
        mov     [exec_ss], ax
        mov     ax, [exe_hdr+MZ_SP]
        mov     [exec_sp], ax
        mov     ax, [exe_hdr+MZ_CS]
        add     ax, [exec_load_seg]
        mov     [exec_cs], ax
        mov     ax, [exe_hdr+MZ_IP]
        mov     [exec_ip], ax
        mov     es, [exec_hdr_seg]
        call    mem_free
        pop     es
        popa
        clc
        ret
.bad:   pop     es
        popa
        mov     ax, 11                          ; invalid format
        stc
        ret
.no_memory:
        cmp     word [exec_hdr_seg], 0
        je      .nm_done
        mov     es, [exec_hdr_seg]
        call    mem_free
.nm_done:
        pop     es
        popa
        mov     ax, 8
        stc
        ret

; build_psp: fill in the PSP at exec_psp
build_psp:
        pusha
        push    es
        mov     es, [exec_psp]
        xor     di, di
        xor     ax, ax
        mov     cx, 128
        rep     stosw
        mov     word [es:0x00], 0x20CD          ; INT 20h
        mov     ax, [exec_psp]
        add     ax, [exec_block_paras]
        mov     [es:PSP_TOP], ax
        mov     byte [es:0x05], 0x9A            ; CALL FAR to the dispatcher
        mov     word [es:0x06], PSP_DISPATCH
        mov     ax, [exec_psp]
        mov     [es:0x08], ax
        mov     word [es:PSP_INT22], int22_handler
        mov     [es:PSP_INT22+2], cs
        mov     word [es:PSP_INT23], int_iret
        mov     [es:PSP_INT23+2], cs
        mov     word [es:PSP_INT24], int24_handler
        mov     [es:PSP_INT24+2], cs
        mov     ax, [cur_psp]
        or      ax, ax
        jnz     .parent
        mov     ax, [exec_psp]
.parent:
        mov     [es:PSP_PARENT], ax
        mov     di, PSP_JFT
        mov     al, 0xFF
        mov     cx, 20
        rep     stosb
        mov     byte [es:PSP_JFT], 1
        mov     byte [es:PSP_JFT+1], 1
        mov     byte [es:PSP_JFT+2], 1
        mov     byte [es:PSP_JFT+3], 0
        mov     byte [es:PSP_JFT+4], 2
        mov     ax, [exec_env_seg]
        mov     [es:PSP_ENV], ax
        mov     word [es:PSP_JFT_SIZE], 20
        mov     word [es:PSP_JFT_PTR], PSP_JFT
        mov     ax, [exec_psp]
        mov     [es:PSP_JFT_PTR+2], ax
        mov     byte [es:PSP_DISPATCH], 0xCD    ; INT 21h ; RETF
        mov     byte [es:PSP_DISPATCH+1], 0x21
        mov     byte [es:PSP_DISPATCH+2], 0xCB
        ; FCBs: blank names
        mov     di, PSP_FCB1
        mov     byte [es:di], 0
        inc     di
        mov     al, ' '
        mov     cx, 11
        rep     stosb
        mov     di, PSP_FCB2
        mov     byte [es:di], 0
        inc     di
        mov     cx, 11
        rep     stosb
        ; command tail
        movzx   cx, byte [exec_tail_len]
        mov     [es:PSP_TAIL], cl
        mov     di, PSP_TAIL+1
        mov     si, exec_tail
        rep     movsb
        mov     byte [es:di], 13
        pop     es
        popa
        ret

; -----------------------------------------------------------------------------
; proc_exit: the current program has ended (exit_code set).  Frees its
;   memory and handles, then resumes the parent.
; -----------------------------------------------------------------------------
proc_exit:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld
        mov     al, 'E'
        call    trace_mark
        mov     ax, [cur_psp]
        call    handles_close_owner
        call    mem_free_owner
        cmp     byte [proc_depth], 0
        je      .to_shell_fallback
        dec     byte [proc_depth]
        movzx   bx, byte [proc_depth]
        imul    bx, PC_SIZE
        add     bx, proc_stack
        mov     ax, [bx+PC_PSP]
        mov     [cur_psp], ax
        mov     ax, [bx+PC_DTA_OFF]
        mov     [dta_off], ax
        mov     ax, [bx+PC_DTA_SEG]
        mov     [dta_seg], ax
        mov     ss, [bx+PC_SS]
        mov     sp, [bx+PC_SP]
        sti
        cmp     byte [bx+PC_TYPE], 0
        je      program_return
        ; back into the parent's INT 21h AH=4Bh frame (on its own stack)
        mov     bp, sp
        and     R_FLAGS, 0xFFFE
        jmp     int21_direct
.to_shell_fallback:
        mov     word [cur_psp], 0
        mov     ss, [saved_ss]
        mov     sp, [saved_sp]
        sti
        jmp     program_return

; program_return: back in the shell (or GUI) context after a top-level program
program_return:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld
        mov     al, 'R'
        call    trace_mark
        mov     word [cur_psp], 0
        mov     byte [proc_depth], 0
        call    mem_free_programs               ; nothing can leak past this point
        mov     al, MOD_EV_END                  ; the modules may want to know
        call    mod_event
        xor     ax, ax
        call    handles_close_owner             ; (owner 0: handles opened by
        call    ivt_restore                     ;  a program with a switched PSP)
        call    spk_bridge_resume               ; listen to the speaker again
        call    kb_hw_reset                     ; a program that read the 8042
        ; the PIT back to 18.2 Hz in case the program reprogrammed it
        mov     al, 0x36
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        ; text mode back if the program changed it
        mov     ah, 0x0F
        int     0x10
        cmp     al, 0x03
        je      .mode_ok
        call    set_text_mode
.mode_ok:
        mov     ax, cs                          ; restore our segment regs
        mov     ds, ax
        mov     es, ax
        clc                                     ; the program ran: success
        ret

; -----------------------------------------------------------------------------
; nx_cleanup: put back what a 32-bit program may have changed, WITHOUT
;   freeing any memory: the program that launched it is still running.
; -----------------------------------------------------------------------------
nx_cleanup:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld
        call    ivt_restore
        mov     ax, [nx_ret_dta_seg]
        mov     [dta_seg], ax
        mov     ax, [nx_ret_dta_off]
        mov     [dta_off], ax
        call    spk_bridge_resume
        call    kb_hw_reset
        mov     al, 0x36                        ; the timer back to 18.2 Hz
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        mov     ah, 0x0F                        ; and text mode, if it moved
        int     0x10
        cmp     al, 0x03
        je      .mode_ok
        call    set_text_mode
.mode_ok:
        ret

; ivt_save / ivt_restore: keep sloppy programs from leaving vectors behind
ivt_save:
        pusha
        push    ds
        push    es
        mov     es, [stk_seg]
        xor     ax, ax
        mov     ds, ax
        xor     si, si
        mov     di, STK_IVT
        mov     cx, 512
        cli
        rep     movsw
        sti
        pop     es
        pop     ds
        popa
        ret

ivt_restore:
        pusha
        push    ds
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ds, [stk_seg]
        mov     si, STK_IVT
        xor     di, di
        mov     cx, 512
        cli
        rep     movsw
        sti
        pop     es
        pop     ds
        popa
        ret

; -----------------------------------------------------------------------------
; run_program_file: used by the shell and the GUI.  DS:SI = path (NUL
;   terminated), DS:DI = argument text (NUL terminated, no leading space).
;   Runs the program to completion.  CF=1 with AX = error if it could not start
; -----------------------------------------------------------------------------
run_program_file:
        push    si
        ; command tail = " " + args (if any)
        mov     si, di
        mov     di, exec_tail
        xor     cx, cx
        cmp     byte [si], 0
        je      .tail_done
        mov     al, ' '
        stosb
        inc     cx
.tail:  lodsb
        or      al, al
        jz      .tail_done
        stosb
        inc     cx
        cmp     cx, 126
        jb      .tail
.tail_done:
        mov     [exec_tail_len], cl
        pop     si
        mov     byte [exec_type], 0
        mov     al, MOD_EV_START                ; the modules may want to know
        call    mod_event
        call    exec_program                    ; CF=1 if it could not start
        jnc     .ran
        push    ax
        mov     al, MOD_EV_END                  ; nothing ran after all
        call    mod_event
        pop     ax
        stc
.ran:   ret

; =============================================================================
; RTC helpers
; =============================================================================
; bcd_to_bin: AL (BCD) -> AL (binary)
bcd_to_bin:
        push    cx
        mov     cl, al
        shr     al, 4
        mov     ch, al
        mov     al, cl
        and     al, 0x0F
        add     al, ch                          ; low + high
        add     al, ch                          ; low + high*2
        shl     ch, 3
        add     al, ch                          ; low + high*10
        pop     cx
        ret

; rtc_read_time: CH = hour, CL = minute, DH = second (binary)
rtc_read_time:
        push    ax
        mov     ah, 0x02
        int     0x1A
        jnc     .ok
        xor     cx, cx
        xor     dx, dx
        jmp     .done
.ok:    mov     al, ch
        call    bcd_to_bin
        mov     ch, al
        mov     al, cl
        call    bcd_to_bin
        mov     cl, al
        mov     al, dh
        call    bcd_to_bin
        mov     dh, al
.done:  pop     ax
        ret

; rtc_read_date: AX = year (e.g. 2026), DH = month, DL = day (binary)
rtc_read_date:
        push    cx
        mov     ah, 0x04
        int     0x1A
        jnc     .ok
        mov     ax, 1980
        mov     dx, 0x0101
        jmp     .done
.ok:    mov     al, dh
        call    bcd_to_bin
        mov     dh, al
        mov     al, dl
        call    bcd_to_bin
        mov     dl, al
        mov     al, ch                          ; century
        call    bcd_to_bin
        mov     ah, 100
        mul     ah                              ; AX = century * 100
        push    ax
        mov     al, cl
        call    bcd_to_bin
        xor     ah, ah
        pop     cx
        add     ax, cx
.done:  pop     cx
        ret

; day_of_week: AX = year, DH = month, DL = day -> AL = 0 (Sunday) .. 6
day_of_week:
        push    bx
        push    cx
        push    dx
        push    si
        mov     bx, ax                          ; BX = year
        cmp     dh, 3
        jae     .no_adjust
        dec     bx
.no_adjust:
        movzx   si, dh
        dec     si
        movzx   cx, byte [dow_table+si]         ; CX = month offset
        movzx   ax, dl
        add     cx, ax                          ; + day
        add     cx, bx                          ; + y
        mov     ax, bx
        shr     ax, 2
        add     cx, ax                          ; + y/4
        mov     ax, bx
        xor     dx, dx
        push    cx
        mov     cx, 100
        div     cx
        pop     cx
        sub     cx, ax                          ; - y/100
        shr     ax, 2
        add     cx, ax                          ; + y/400
        mov     ax, cx
        xor     dx, dx
        mov     cx, 7
        div     cx
        mov     al, dl
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

dow_table:      db 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4

; -----------------------------------------------------------------------------
section .data
cur_psp:        dw 0
proc_depth:     db 0
trace_flag:     db 0
indos_flag:     db 0
pending_key:    db 0
exit_code:      db 0
exec_type:      db 0
nx_ret_type:    db 0                            ; how a 32-bit program was started
path_api:       db 0                            ; inside an INT 21h path
exec_is_exe:    db 0
exec_tail_len:  db 0
saved_ss:       dw 0
saved_sp:       dw 0
dta_off:        dw 0
dta_seg:        dw 0
exec_path_ptr:  dw 0
exec_cluster:   dw 0
exec_date:      dw 0
exec_env_seg:   dw 0
exec_psp:       dw 0
exec_block_paras: dw 0
exec_load_seg:  dw 0
exec_hdr_seg:   dw 0
exec_image_paras: dw 0
exec_ss:        dw 0
exec_sp:        dw 0
exec_cs:        dw 0
exec_ip:        dw 0
ff_dir:         dw 0
ff_last_sep:    dw 0
ff_index:       dw 0
                align 4
exec_size:      dd 0
exec_image_size: dd 0
                dw 0                            ; first MCB segment (for 52h)
sysvars:        times 32 db 0
env_blaster_on: db 0
name_sb:        db "SB      "
name_dpmi:      db "DPMI    "
; What a game's setup reads to find the card: base 220h, interrupt 5,
; transfer channel 1, and a type of 3 - a Sound Blaster Pro, which is what
; the DSP here says it is.
env_blaster:    db "BLASTER=A220 I5 D1 T3", 0
env_blaster_end:
master_env:     db "PATH=C:\", 0
                db "COMSPEC=C:\COMMAND.COM", 0
                db "PROMPT=$P$G", 0
                db 0
msg_prog_too_big: db "Program too large to load.", 13, 10, 0
msg_disk_error:   db "Disk read error.", 13, 10, 0
section .bss
handles:        resb MAX_HANDLES * HANDLE_SIZE
proc_stack:     resb MAX_PROC * PC_SIZE
hr_count:       resd 1
hr_total:       resd 1
hr_off:         resd 1
w_src_seg:      resw 1
w_src_off:      resw 1
w_count:        resd 1
w_total:        resd 1
w_off:          resd 1
w_sec_off:      resd 1
w_n:            resd 1
w_lba:          resd 1
ext_action:     resb 2
line_buf:       resb 130
path_buf:       resb 80
exec_tail:      resb 128
exec_full:      resb 96
exe_hdr:        resb 32
ff_pattern:     resb 12
caller_ss:      resw 1
caller_sp:      resw 1
nx_ret_ss:      resw 1                          ; the parent's INT 21h frame
nx_ret_sp:      resw 1
nx_ret_dta_seg: resw 1
nx_ret_dta_off: resw 1
section .text
