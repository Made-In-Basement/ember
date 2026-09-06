; =============================================================================
;  I2C.COM - talk to the touchpad and touchscreen over I2C
; -----------------------------------------------------------------------------
;  On the Yoga 3 Pro both touch devices are "HID over I2C" behind Intel's
;  Serial IO controllers, which are DesignWare I2C blocks at fixed memory
;  addresses (from Windows: FE103000 and FE105000).  This asks each of them
;  three things, in order, and writes everything down in C:\I2C.TXT:
;
;    1. Is the controller there and powered?  Its identity register reads
;       44570140h ("DW" + a version) when it is.  If not, the power-on
;       sequence from the firmware's own tables is tried.
;    2. Does the device answer?  The HID descriptor is read: 30 bytes that
;       begin 1E 00 00 01 and end with the maker's number (06CBh Synaptics,
;       03EBh Atmel).
;    3. Does it report?  It is switched on and reset, then polled for three
;       seconds while you touch it, and the first reports are listed.
;
;  Memory above 1 MB is reached from real mode by giving FS a 4 GB limit
;  (the trick the kernel's sound driver uses).  A bounded wait is used
;  everywhere, so a machine without these controllers just says so.
; =============================================================================

[BITS 16]
[ORG 0x0100]

TICKS           equ 0x046C

; ---- the DesignWare I2C registers ----
IC_CON          equ 0x00
IC_TAR          equ 0x04
IC_DATA_CMD     equ 0x10
IC_SS_SCL_HCNT  equ 0x14
IC_SS_SCL_LCNT  equ 0x18
IC_FS_SCL_HCNT  equ 0x1C
IC_FS_SCL_LCNT  equ 0x20
IC_INTR_MASK    equ 0x30
IC_RAW_INTR_STAT equ 0x34
IC_RX_TL        equ 0x38
IC_TX_TL        equ 0x3C
IC_CLR_INTR     equ 0x40
IC_CLR_TX_ABRT  equ 0x54
IC_ENABLE       equ 0x6C
IC_STATUS       equ 0x70
IC_TXFLR        equ 0x74
IC_RXFLR        equ 0x78
IC_SDA_HOLD     equ 0x7C
IC_TX_ABRT_SRC  equ 0x80
IC_ENABLE_STATUS equ 0x9C
IC_COMP_PARAM_1 equ 0xF4
IC_COMP_VERSION equ 0xF8
IC_COMP_TYPE    equ 0xFC
DW_IDENT        equ 0x44570140

; ---- Intel's private registers behind the block ----
PRV_CLOCK       equ 0x800                       ; bit 0: clock on
PRV_RESETS      equ 0x804                       ; bits 0-1 set: out of reset
PRV_GENERAL     equ 0x808
PRV_POWER       equ 0x884                       ; bits 0-1: 0 = on, 3 = off

CMD_READ        equ 0x100
CMD_STOP        equ 0x200
CMD_RESTART     equ 0x400

SPIN_MAX        equ 400000                      ; polls before giving up
REPORT_MAX      equ 12                          ; reports listed per device
POLL_TICKS      equ 55                          ; three seconds of touching

start:
        mov     di, bss_start                   ; a .COM starts with whatever
        mov     cx, bss_end - bss_start         ;  was in memory before it
        xor     al, al
        cld
        rep     stosb

        xor     ax, ax
        mov     fs, ax                          ; the BIOS clock, until unreal
        mov     si, msg_head
        call    puts

        ; Prove the path to high memory works before blaming a controller:
        ; the HD Audio block's first register (from PCI.TXT its region is
        ; C131C000h) reads as a small non-zero capability word.
        mov     si, msg_selftest
        call    puts
        mov     dword [cur_base], 0xC131C000
        xor     bx, bx
        call    ic_rd
        call    print_hex32
        mov     si, msg_selftest2
        call    puts
        mov     dword [cur_base], 0xFFFFFFF0     ; the reset vector in the BIOS ROM
        xor     bx, bx
        call    ic_rd
        call    print_hex32
        call    crlf

        call    write_log
        call    chipset_dump
        call    write_log
        call    scan_blocks
        call    write_log

        ; ---- the touchpad: host 0, address 2Ch, descriptor at 20h ----
        mov     si, msg_touchpad
        call    puts
        mov     eax, [found_i2c]                ; wherever the sweep found it
        test    eax, eax
        jnz     .tp_base
        mov     eax, 0xFE103000
.tp_base:
        mov     [cur_base], eax
        mov     word [dev_addr], 0x2C
        mov     word [desc_reg], 0x0020
        call    probe_device
        call    write_log

        ; ---- the touchscreen: host 1, address 4Ah, descriptor at 0 ----
        mov     si, msg_touchscreen
        call    puts
        mov     eax, [found_i2c+4]
        test    eax, eax
        jnz     .ts_base
        mov     eax, 0xFE105000
.ts_base:
        mov     [cur_base], eax
        mov     word [dev_addr], 0x4A
        mov     word [desc_reg], 0x0000
        call    probe_device

        mov     si, msg_done
        call    puts
        call    write_log
        mov     ax, 0x4C00
        int     0x21

; =============================================================================
; probe_device: the three questions, for [cur_base] / [dev_addr] / [desc_reg]
; =============================================================================
probe_device:
        ; ---- 1. the controller ----
        call    host_dump
        mov     bx, IC_COMP_TYPE
        call    ic_rd
        cmp     eax, DW_IDENT
        je      .host_ok
        mov     si, msg_powering
        call    puts
        call    host_power_on
        call    host_dump
        mov     bx, IC_COMP_TYPE
        call    ic_rd
        cmp     eax, DW_IDENT
        je      .host_ok
        mov     si, msg_no_host
        call    puts
        ret
.host_ok:
        call    host_power_on                   ; harmless when already on
        call    host_setup

        ; ---- 2. the HID descriptor ----
        mov     si, msg_desc
        call    puts
        mov     ax, [desc_reg]
        mov     [wr_buf], al
        mov     [wr_buf+1], ah
        mov     word [wr_len], 2
        mov     word [rd_len], 30
        call    i2c_xfer
        jc      .no_answer
        mov     si, rd_buf
        mov     cx, 30
        call    hex_dump
        call    decode_desc
        cmp     word [rd_buf], 0x001E           ; wHIDDescLength
        jne     .odd_desc

        ; ---- 3. reports ----
        call    hid_wake
        call    hid_poll
        ret
.odd_desc:
        mov     si, msg_odd_desc
        call    puts
        ret
.no_answer:
        mov     si, msg_no_answer
        call    puts
        call    print_abort
        ret

; ---------------------------------------------------------------- the chipset
; chipset_dump: what the PCH says about these functions.  The LPC bridge
;   (00:1F.0) holds the root complex base (RCBA) at config F0h; the
;   function-disable registers live at RCBA+3418h and +3428h.  The Serial
;   IO functions are device 21 (15h); their vendor words say whether they
;   are visible at all.
chipset_dump:
        mov     si, msg_pci_sio
        call    puts
        xor     cx, cx                          ; function 0..7 of 00:15
.fn:    mov     bx, 0x00A8                      ; bus 0, device 21, function CL
        or      bl, cl
        xor     al, al
        call    pci_rd
        call    print_hex32
        mov     al, ' '
        call    putc
        inc     cx
        cmp     cx, 8
        jb      .fn
        call    crlf
        mov     si, msg_pci_lpc
        call    puts
        mov     bx, 0x00F8                      ; 00:1F.0
        xor     al, al
        call    pci_rd
        call    print_hex32
        mov     si, msg_rcba
        call    puts
        mov     bx, 0x00F8
        mov     al, 0xF0
        call    pci_rd
        mov     [rcba], eax
        call    print_hex32
        call    crlf
        cmp     eax, 0xFFFFFFFF                 ; no such bridge (the emulator)
        je      .no_rcba
        test    al, 1                           ; enabled?
        jz      .no_rcba
        and     dword [rcba], 0xFFFFC000
        mov     eax, [rcba]
        mov     [cur_base], eax
        mov     si, msg_fd
        call    puts
        mov     bx, 0x3410                      ; GCS
        call    ic_rd
        call    print_hex32
        mov     al, ' '
        call    putc
        mov     bx, 0x3418                      ; FD
        call    ic_rd
        call    print_hex32
        mov     al, ' '
        call    putc
        mov     bx, 0x3428                      ; FD2
        call    ic_rd
        call    print_hex32
        call    crlf
.no_rcba:
        ret

; pci_rd: BX = bus:dev:fn (bus in BH, dev<<3|fn in BL), AL = register -> EAX
pci_rd:
        push    dx
        push    ebx
        push    ecx
        movzx   ecx, al
        and     ecx, 0xFC
        movzx   eax, bx
        movzx   ebx, bh
        shl     ebx, 16
        and     eax, 0xFF
        shl     eax, 8
        or      eax, ebx
        or      eax, ecx
        or      eax, 0x80000000
        mov     dx, 0x0CF8
        out     dx, eax
        mov     dx, 0x0CFC
        in      eax, dx
        pop     ecx
        pop     ebx
        pop     dx
        ret

; scan_blocks: every 4 KB page of the chipset's memory window, looking for
;   the I2C block's identity at +FCh and noting what else answers there.
scan_blocks:
        mov     si, msg_scan
        call    puts
        mov     dword [cur_base], 0xFE000000
        mov     word [found_n], 0
        mov     word [live_n], 0
.page:  mov     bx, 0xFC
        call    ic_rd
        cmp     eax, DW_IDENT
        jne     .not_i2c
        mov     si, msg_found
        call    puts
        mov     eax, [cur_base]
        call    print_hex32
        call    crlf
        mov     bx, [found_n]
        cmp     bx, 4
        jae     .not_i2c
        shl     bx, 2
        mov     [found_i2c+bx], eax
        inc     word [found_n]
.not_i2c:
        xor     bx, bx
        call    ic_rd
        cmp     eax, 0xFFFFFFFF
        je      .next
        inc     word [live_n]
        cmp     word [live_n], 24
        ja      .next
        mov     si, msg_live
        call    puts
        push    eax
        mov     eax, [cur_base]
        call    print_hex32
        mov     al, ' '
        call    putc
        pop     eax
        call    print_hex32
        call    crlf
.next:  add     dword [cur_base], 0x1000
        cmp     dword [cur_base], 0xFE400000
        jb      .page
        mov     si, msg_scan_end
        call    puts
        mov     ax, [live_n]
        call    print_dec
        mov     si, msg_scan_end2
        call    puts
        mov     ax, [found_n]
        call    print_dec
        call    crlf
        ret

; ---------------------------------------------------------------- the host
; host_dump: the identity and private registers, one line
host_dump:
        mov     si, msg_host_at
        call    puts
        mov     eax, [cur_base]
        call    print_hex32
        mov     si, msg_ident
        call    puts
        mov     bx, IC_COMP_TYPE
        call    ic_rd
        call    print_hex32
        mov     si, msg_version
        call    puts
        mov     bx, IC_COMP_VERSION
        call    ic_rd
        call    print_hex32
        mov     si, msg_param
        call    puts
        mov     bx, IC_COMP_PARAM_1
        call    ic_rd
        call    print_hex32
        call    crlf
        mov     si, msg_prv
        call    puts
        mov     bx, PRV_CLOCK
        call    ic_rd
        call    print_hex32
        mov     si, msg_sp
        call    puts
        mov     bx, PRV_RESETS
        call    ic_rd
        call    print_hex32
        mov     si, msg_sp
        call    puts
        mov     bx, PRV_GENERAL
        call    ic_rd
        call    print_hex32
        mov     si, msg_sp
        call    puts
        mov     bx, PRV_POWER
        call    ic_rd
        call    print_hex32
        call    crlf
        ; the first registers, raw, in case the identity is elsewhere
        mov     si, msg_raw
        call    puts
        xor     bx, bx
.raw:   call    ic_rd
        call    print_hex32
        mov     al, ' '
        call    putc
        add     bx, 4
        cmp     bx, 0x20
        jb      .raw
        call    crlf
        mov     si, msg_raw2
        call    puts
        mov     bx, 0x800
.raw2:  call    ic_rd
        call    print_hex32
        mov     al, ' '
        call    putc
        add     bx, 4
        cmp     bx, 0x820
        jb      .raw2
        call    crlf
        ret

; host_power_on: what the firmware's _PS0 does (clear the two power bits),
;   then take the block out of reset and turn its clock on
host_power_on:
        mov     bx, PRV_POWER
        call    ic_rd
        and     eax, 0xFFFFFFFC
        call    ic_wr
        mov     bx, PRV_RESETS
        mov     eax, 3
        call    ic_wr
        mov     bx, PRV_CLOCK
        call    ic_rd
        or      eax, 1
        call    ic_wr
        ; give it a moment
        mov     bx, 2
        call    wait_ticks
        ret

; host_setup: standard speed (100 kHz) from a 100 MHz clock, master only,
;   restarts allowed, interrupts unused (we poll)
host_setup:
        call    host_disable
        mov     bx, IC_CON
        mov     eax, 0x63                       ; master, standard, restart, no slave
        call    ic_wr
        mov     bx, IC_SS_SCL_HCNT
        mov     eax, 0x1AB
        call    ic_wr
        mov     bx, IC_SS_SCL_LCNT
        mov     eax, 0x1F3
        call    ic_wr
        mov     bx, IC_FS_SCL_HCNT
        mov     eax, 0x57
        call    ic_wr
        mov     bx, IC_FS_SCL_LCNT
        mov     eax, 0x9F
        call    ic_wr
        mov     bx, IC_SDA_HOLD
        mov     eax, 0x1E
        call    ic_wr
        mov     bx, IC_RX_TL
        xor     eax, eax
        call    ic_wr
        mov     bx, IC_TX_TL
        call    ic_wr
        mov     bx, IC_INTR_MASK
        call    ic_wr
        movzx   eax, word [dev_addr]
        mov     bx, IC_TAR
        call    ic_wr
        ret

host_disable:
        mov     bx, IC_ENABLE
        xor     eax, eax
        call    ic_wr
        mov     ecx, SPIN_MAX
.wait:  mov     bx, IC_ENABLE_STATUS
        call    ic_rd
        test    al, 1
        jz      .off
        dec     ecx
        jnz     .wait
.off:   ret

host_enable:
        mov     bx, IC_ENABLE
        mov     eax, 1
        call    ic_wr
        mov     ecx, SPIN_MAX
.wait:  mov     bx, IC_ENABLE_STATUS
        call    ic_rd
        test    al, 1
        jnz     .on
        dec     ecx
        jnz     .wait
.on:    ret

; ---------------------------------------------------------------- a transfer
; i2c_xfer: write [wr_len] bytes of wr_buf to the device, then read
;   [rd_len] bytes into rd_buf (with a restart between).  CF=1 on an
;   abort or a timeout; [abort_src] says which.
i2c_xfer:
        pushad
        mov     dword [abort_src], 0
        mov     word [sent], 0
        mov     word [got], 0
        mov     ax, [wr_len]
        add     ax, [rd_len]
        mov     [total], ax
        call    host_enable
        mov     bx, IC_CLR_INTR
        call    ic_rd
        mov     bx, IC_CLR_TX_ABRT
        call    ic_rd
        mov     ecx, SPIN_MAX
.loop:  ; an abort ends it
        mov     bx, IC_RAW_INTR_STAT
        call    ic_rd
        test    eax, 1 << 6
        jnz     .aborted
        ; something to send, and room to send it?
        mov     ax, [sent]
        cmp     ax, [total]
        jae     .receive
        mov     bx, IC_TXFLR
        call    ic_rd
        cmp     eax, 8
        jae     .receive
        call    next_command                    ; -> EAX
        mov     bx, IC_DATA_CMD
        call    ic_wr
        inc     word [sent]
.receive:
        mov     bx, IC_RXFLR
        call    ic_rd
        test    eax, eax
        jz      .check
        mov     bx, IC_DATA_CMD
        call    ic_rd
        mov     bx, [got]
        cmp     bx, [rd_len]
        jae     .check
        mov     [rd_buf+bx], al
        inc     word [got]
.check: mov     ax, [sent]
        cmp     ax, [total]
        jb      .again
        mov     ax, [got]
        cmp     ax, [rd_len]
        jb      .again
        ; everything sent and received: wait for the bus to go quiet
        mov     bx, IC_STATUS
        call    ic_rd
        test    al, 0x01                        ; ACTIVITY
        jz      .done
.again: dec     ecx
        jnz     .loop
        mov     dword [abort_src], 0xFFFFFFFF   ; our word for a timeout
        call    host_disable
        popad
        stc
        ret
.aborted:
        mov     bx, IC_TX_ABRT_SRC
        call    ic_rd
        mov     [abort_src], eax
        mov     bx, IC_CLR_TX_ABRT
        call    ic_rd
        call    host_disable
        popad
        stc
        ret
.done:  popad
        clc
        ret

; next_command: the [sent]th entry for the data/command register -> EAX
next_command:
        push    bx
        mov     bx, [sent]
        cmp     bx, [wr_len]
        jae     .read
        movzx   eax, byte [wr_buf+bx]           ; a byte to write
        jmp     .last
.read:  mov     eax, CMD_READ
        cmp     bx, [wr_len]
        jne     .last
        cmp     word [wr_len], 0
        je      .last
        or      eax, CMD_RESTART                ; the first read after writes
.last:  inc     bx
        cmp     bx, [total]
        jne     .out
        or      eax, CMD_STOP
.out:   pop     bx
        ret

print_abort:
        mov     si, msg_abort
        call    puts
        mov     eax, [abort_src]
        cmp     eax, 0xFFFFFFFF
        jne     .code
        mov     si, msg_timeout
        call    puts
        ret
.code:  call    print_hex32
        test    eax, 1                          ; bit 0: address not acknowledged
        jz      .nl
        mov     si, msg_no_ack
        call    puts
.nl:    call    crlf
        ret

; ---------------------------------------------------------------- HID over I2C
; decode_desc: the interesting fields of the 30 bytes in rd_buf
decode_desc:
        mov     si, msg_d_len
        call    puts
        mov     ax, [rd_buf+0]
        call    print_dec
        mov     si, msg_d_ver
        call    puts
        mov     ax, [rd_buf+2]
        call    print_hex16
        mov     si, msg_d_rdesc
        call    puts
        mov     ax, [rd_buf+4]
        call    print_dec
        mov     si, msg_d_input
        call    puts
        mov     ax, [rd_buf+8]
        mov     [input_reg], ax
        call    print_hex16
        mov     si, msg_d_maxin
        call    puts
        mov     ax, [rd_buf+10]
        mov     [max_input], ax
        call    print_dec
        mov     si, msg_d_cmd
        call    puts
        mov     ax, [rd_buf+16]
        mov     [cmd_reg], ax
        call    print_hex16
        mov     si, msg_d_data
        call    puts
        mov     ax, [rd_buf+18]
        mov     [data_reg], ax
        call    print_hex16
        mov     si, msg_d_vendor
        call    puts
        mov     ax, [rd_buf+20]
        call    print_hex16
        mov     si, msg_d_product
        call    puts
        mov     ax, [rd_buf+22]
        call    print_hex16
        mov     si, msg_d_version
        call    puts
        mov     ax, [rd_buf+24]
        call    print_hex16
        call    crlf
        ret

; hid_wake: SET_POWER on, then RESET, as the specification asks
hid_wake:
        mov     si, msg_wake
        call    puts
        mov     ax, [cmd_reg]
        mov     [wr_buf], al
        mov     [wr_buf+1], ah
        mov     byte [wr_buf+2], 0x00           ; power state 0 = on
        mov     byte [wr_buf+3], 0x08           ; SET_POWER
        mov     word [wr_len], 4
        mov     word [rd_len], 0
        call    i2c_xfer
        jc      .failed
        mov     bx, 2
        call    wait_ticks
        mov     ax, [cmd_reg]
        mov     [wr_buf], al
        mov     [wr_buf+1], ah
        mov     byte [wr_buf+2], 0x00
        mov     byte [wr_buf+3], 0x01           ; RESET
        call    i2c_xfer
        jc      .failed
        mov     bx, 4                           ; the reset takes a moment
        call    wait_ticks
        mov     si, msg_ok
        call    puts
        ret
.failed:
        mov     si, msg_failed
        call    puts
        call    print_abort
        ret

; hid_poll: read the input register for a while and list what arrives.
;   A report is [length lo][length hi][bytes...]; zero length is the
;   reset's acknowledgement, and an idle device says nothing at all.
hid_poll:
        mov     si, msg_touch_now
        call    puts_screen
        mov     word [reports], 0
        mov     word [empties], 0
        mov     word [errors], 0
        mov     eax, [fs:TICKS]
        add     eax, POLL_TICKS
        mov     [poll_end], eax
.poll:  mov     word [wr_len], 0
        mov     ax, [max_input]
        cmp     ax, 64
        jbe     .sized
        mov     ax, 64
.sized: mov     [rd_len], ax
        call    i2c_xfer
        jc      .err
        mov     ax, [rd_buf]                    ; the length the device gives
        test    ax, ax
        jz      .empty
        cmp     ax, 64
        ja      .empty                          ; nonsense: FFFF from a dead line
        inc     word [reports]
        cmp     word [reports], REPORT_MAX
        ja      .next
        push    ax
        mov     si, msg_report
        call    puts
        pop     ax
        push    ax
        call    print_dec
        mov     si, msg_colon
        call    puts
        pop     cx
        cmp     cx, 24
        jbe     .show
        mov     cx, 24
.show:  mov     si, rd_buf
        call    hex_dump
        jmp     .next
.empty: inc     word [empties]
        jmp     .next
.err:   inc     word [errors]
        cmp     word [errors], 1
        jne     .next
        call    print_abort                     ; the first one, in full
.next:  sti
        hlt                                     ; about 55 ms between polls
        mov     eax, [fs:TICKS]
        cmp     eax, [poll_end]
        jb      .poll
        mov     si, msg_poll_sum
        call    puts
        mov     ax, [reports]
        call    print_dec
        mov     si, msg_poll_sum2
        call    puts
        mov     ax, [empties]
        call    print_dec
        mov     si, msg_poll_sum3
        call    puts
        mov     ax, [errors]
        call    print_dec
        call    crlf
        ret

; ---------------------------------------------------------------- memory
; ic_rd: [cur_base] + BX -> EAX.   ic_wr: EAX -> [cur_base] + BX
ic_rd:
        push    ebx
        pushf
        cli                                     ; nobody reloads FS meanwhile
        call    go_unreal
        movzx   ebx, bx
        add     ebx, [cur_base]
        mov     eax, [fs:ebx]
        popf
        pop     ebx
        ret

ic_wr:
        push    ebx
        pushf
        cli
        call    go_unreal
        movzx   ebx, bx
        add     ebx, [cur_base]
        mov     [fs:ebx], eax
        popf
        pop     ebx
        ret

; go_unreal: give FS a 4 GB limit.  Done before every access, because
;   anything that reloads FS in real mode (an interrupt handler, the BIOS)
;   puts the 64 KB limit back.
go_unreal:
        cmp     byte [unreal_ok], 0
        jne     .quick
        push    eax
        mov     ax, cs
        movzx   eax, ax
        shl     eax, 4
        add     eax, gdt
        mov     [gdt_ptr+2], eax
        pop     eax
        mov     byte [unreal_ok], 1
.quick: push    eax
        push    bx
        pushf
        cli
        lgdt    [gdt_ptr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     short .pm
.pm:    mov     bx, 0x08
        mov     fs, bx
        and     al, 0xFE
        mov     cr0, eax
        jmp     short .rm
.rm:    xor     bx, bx                          ; base 0, limit now 4 GB
        mov     fs, bx
        popf
        pop     bx
        pop     eax
        ret

gdt:    dq 0
        dq 0x00CF92000000FFFF                   ; flat 4 GB data
gdt_ptr:
        dw 15
        dd 0

; ---------------------------------------------------------------- time
wait_ticks:
        push    eax
        push    ebx
        movzx   ebx, bx
        add     ebx, [fs:TICKS]
.spin:  sti
        hlt
        mov     eax, [fs:TICKS]
        cmp     eax, ebx
        jb      .spin
        pop     ebx
        pop     eax
        ret

; ---------------------------------------------------------------- the file
write_log:
        mov     dx, log_name
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .failed
        mov     bx, ax
        mov     cx, [log_len]
        mov     dx, log_buf
        mov     ah, 0x40
        int     0x21
        pushf
        push    ax
        mov     ah, 0x3E
        int     0x21
        pop     ax
        popf
        jc      .short
        cmp     ax, [log_len]
        jne     .short
        mov     si, msg_saved
        call    puts_screen
        call    print_dec_screen
        mov     si, msg_bytes
        call    puts_screen
        ret
.short: mov     si, msg_short
        call    puts_screen
        call    print_dec_screen
        mov     si, msg_bytes
        call    puts_screen
        ret
.failed:
        mov     si, msg_nosave
        call    puts_screen
        ret

; print_dec_screen: AX in decimal, screen only
print_dec_screen:
        push    bx
        mov     bx, [log_len]
        push    bx
        mov     word [log_len], LOG_MAX         ; so nothing lands in the log
        call    print_dec
        pop     bx
        mov     [log_len], bx
        pop     bx
        ret

; ---------------------------------------------------------------- printing
; hex_dump: CX bytes at DS:SI, spaced, with a line break
hex_dump:
        push    ax
        push    cx
        push    si
.next:  lodsb
        call    print_hex8
        mov     al, ' '
        call    putc
        loop    .next
        call    crlf
        pop     si
        pop     cx
        pop     ax
        ret

puts:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:  pop     ax
        ret

puts_screen:
        push    ax
.loop:  lodsb
        or      al, al
        jz      .done
        call    putc_screen
        jmp     .loop
.done:  pop     ax
        ret

putc:
        call    putc_screen
        push    bx
        mov     bx, [log_len]
        cmp     bx, LOG_MAX - 1
        jae     .full
        mov     [log_buf+bx], al
        inc     word [log_len]
.full:  pop     bx
        ret

putc_screen:
        push    ax
        push    bx
        mov     ah, 0x0E
        xor     bx, bx
        int     0x10
        pop     bx
        pop     ax
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

print_dec:
        push    eax
        push    ebx
        push    ecx
        push    edx
        movzx   eax, ax
        mov     ebx, 10
        xor     cx, cx
        test    eax, eax
        jnz     .split
        mov     al, '0'
        call    putc
        jmp     .out
.split: xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .split
.emit:  pop     ax
        add     al, '0'
        call    putc
        loop    .emit
.out:   pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

print_hex32:
        push    eax
        shr     eax, 16
        call    print_hex16
        pop     eax
        call    print_hex16
        ret

print_hex16:
        push    ax
        mov     al, ah
        call    print_hex8
        pop     ax
        call    print_hex8
        ret

print_hex8:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .digit
        mov     al, cl
        and     al, 0x0F
        call    .digit
        pop     cx
        pop     ax
        ret
.digit: and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:   call    putc
        ret

msg_head:       db "The touch devices, over I2C", 13, 10
                db "===========================", 13, 10, 0
msg_selftest:   db "High memory check: sound chip reads ", 0
msg_selftest2:  db ", BIOS ROM reads ", 0
msg_raw:        db "  registers 00-1C: ", 0
msg_raw2:       db "  private 800-81C: ", 0
msg_pci_sio:    db "PCI 00:15.0-7 (Serial IO) vendor/device: ", 0
msg_pci_lpc:    db "PCI 00:1F.0 (LPC): ", 0
msg_rcba:       db "  RCBA ", 0
msg_fd:         db "  GCS/FD/FD2: ", 0
msg_scan:       db "Sweeping FE000000-FE3FFFFF for I2C blocks...", 13, 10, 0
msg_found:      db "  I2C block at ", 0
msg_live:       db "  something answers at ", 0
msg_scan_end:   db "  pages answering: ", 0
msg_scan_end2:  db ", I2C blocks: ", 0
msg_touchpad:   db 13, 10, "Touchpad (Synaptics, host 0, address 2Ch)", 13, 10, 0
msg_touchscreen: db 13, 10, "Touchscreen (Atmel, host 1, address 4Ah)", 13, 10, 0
msg_host_at:    db "  host at ", 0
msg_ident:      db ": identity ", 0
msg_version:    db " version ", 0
msg_param:      db " param ", 0
msg_prv:        db "  private: clock/resets/general/power ", 0
msg_sp:         db " ", 0
msg_powering:   db "  not answering: trying the power-on sequence", 13, 10, 0
msg_no_host:    db "  no controller reachable here", 13, 10, 0
msg_desc:       db "  HID descriptor: ", 0
msg_no_answer:  db "no answer", 13, 10, 0
msg_abort:      db "  transfer aborted: ", 0
msg_timeout:    db "timed out", 13, 10, 0
msg_no_ack:     db " (address not acknowledged)", 0
msg_odd_desc:   db "  that is not a HID descriptor (should begin 1E 00)", 13, 10, 0
msg_d_len:      db "    length ", 0
msg_d_ver:      db "  version ", 0
msg_d_rdesc:    db "  report descriptor ", 0
msg_d_input:    db " bytes  input reg ", 0
msg_d_maxin:    db "  max input ", 0
msg_d_cmd:      db "  command reg ", 0
msg_d_data:     db "  data reg ", 0
msg_d_vendor:   db 13, 10, "    vendor ", 0
msg_d_product:  db "  product ", 0
msg_d_version:  db "  version ", 0
msg_wake:       db "  power on and reset: ", 0
msg_ok:         db "ok", 13, 10, 0
msg_failed:     db "failed", 13, 10, 0
msg_touch_now:  db "  Touch it now, for three seconds...", 13, 10, 0
msg_report:     db "  report, ", 0
msg_colon:      db " bytes: ", 0
msg_poll_sum:   db "  reports ", 0
msg_poll_sum2:  db ", empty reads ", 0
msg_poll_sum3:  db ", failed reads ", 0
msg_done:       db 13, 10, "Done.", 13, 10, 0
msg_saved:      db "Written to C:\I2C.TXT, ", 0
msg_short:      db "C:\I2C.TXT: the write fell short, ", 0
msg_bytes:      db " bytes", 13, 10, 0
msg_nosave:     db "Could not write C:\I2C.TXT", 13, 10, 0
log_name:       db "\I2C.TXT", 0

LOG_MAX         equ 12288

section .bss
bss_start:
cur_base:       resd 1
dev_addr:       resw 1
desc_reg:       resw 1
input_reg:      resw 1
max_input:      resw 1
cmd_reg:        resw 1
data_reg:       resw 1
wr_len:         resw 1
rd_len:         resw 1
total:          resw 1
sent:           resw 1
got:            resw 1
abort_src:      resd 1
poll_end:       resd 1
reports:        resw 1
empties:        resw 1
errors:         resw 1
unreal_ok:      resb 1
rcba:           resd 1
found_i2c:      resd 4
found_n:        resw 1
live_n:         resw 1
wr_buf:         resb 8
rd_buf:         resb 64
log_len:        resw 1
log_buf:        resb LOG_MAX
bss_end:
