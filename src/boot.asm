; =============================================================================
;  Ember boot sector  (src/boot.asm)
; -----------------------------------------------------------------------------
;  A 512-byte FAT12/FAT16 volume boot record.  The BIOS loads it at 0000:7C00
;  and jumps to it with DL = boot drive.  It loads the kernel, which lives in
;  the reserved sectors of the volume (LBA 1 .. reserved_sectors-1) so that the
;  filesystem stays valid and Windows can still mount the USB stick.
;
;  Disk access: INT 13h extensions (LBA, AH=42h) when available, otherwise
;  classic CHS (AH=02h) using the geometry the BIOS reports (AH=08h).
;  Both paths matter on real hardware: USB-HDD emulation gives LBA, while
;  USB-FDD emulation and real floppies only give CHS.
; =============================================================================

[BITS 16]
[ORG 0x7C00]

KERNEL_SEG      equ 0x0800          ; kernel is loaded at 0800:0000 (phys 0x08000)

; --- Entry: the standard "jmp short / nop" that precedes a BPB ---------------
        jmp short start
        nop

; --- BIOS Parameter Block (values here are placeholders; build.py patches) ---
bpb_oem:            db "EMBER   "  ; OEM name (exactly 8 bytes)
bpb_bytes_per_sec:  dw 512
bpb_sec_per_clus:   db 1
bpb_reserved_secs:  dw 1            ; 1 + number of kernel sectors (patched)
bpb_num_fats:       db 2
bpb_root_entries:   dw 224
bpb_total_secs16:   dw 2880
bpb_media:          db 0xF0
bpb_secs_per_fat:   dw 9
bpb_secs_per_track: dw 18
bpb_heads:          dw 2
bpb_hidden_secs:    dd 0            ; LBA of this volume on the disk (partition offset)
bpb_total_secs32:   dd 0
; --- Extended BPB ---
ebpb_drive:         db 0x80
ebpb_reserved:      db 0
ebpb_signature:     db 0x29
ebpb_volume_id:     dd 0x4E414E4F
ebpb_volume_label:  db "EMBER      "  ; exactly 11 bytes
ebpb_fs_type:       db "FAT12   "       ; 8 bytes

; The BPB is a fixed 62-byte header: build.py writes its own over bytes 3..61,
; so if anything above changes size the first instructions get overwritten and
; the disk simply will not boot.  Catch that here instead of on the machine.
%if ($ - $$) != 62
  %error "the BIOS parameter block is not 62 bytes: check the string widths"
%endif

; =============================================================================
start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00              ; stack grows down from just below us
        sti
        cld
        jmp     0x0000:.normalized      ; some BIOSes jump to 07C0:0000
.normalized:
        mov     [boot_drive], dl

        mov     si, msg_banner
        call    print

        ; ---- Do we have INT 13h extensions (LBA reads)? ---------------------
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [boot_drive]
        int     0x13
        jc      .no_lba
        cmp     bx, 0xAA55
        jne     .no_lba
        test    cx, 1                   ; bit 0: fixed-disk access subset
        jz      .no_lba
        mov     byte [use_lba], 1
.no_lba:

        ; ---- Ask the BIOS for the drive geometry (for CHS fallback) ---------
        mov     ah, 0x08
        mov     dl, [boot_drive]
        xor     di, di                  ; ES:DI = 0 works around buggy BIOSes
        int     0x13
        jc      .geom_done
        and     cx, 0x3F                ; CL[5:0] = sectors per track
        jz      .geom_done
        mov     [bpb_secs_per_track], cx
        mov     dl, dh                  ; DH = last head index
        xor     dh, dh
        inc     dx
        mov     [bpb_heads], dx
.geom_done:
        xor     ax, ax
        mov     es, ax                  ; AH=08h clobbers ES:DI

        ; ---- Load the kernel: LBA 1 .. reserved_secs-1 -> 0800:0000 ----------
        mov     ax, [bpb_reserved_secs]
        dec     ax
        mov     [sectors_left], ax
        mov     eax, 1
        add     eax, [bpb_hidden_secs]
        mov     bx, KERNEL_SEG
        mov     es, bx
        xor     bx, bx
.load_loop:
        cmp     word [sectors_left], 0
        je      .loaded
        call    read_sector
        add     bx, 512
        inc     eax
        dec     word [sectors_left]
        mov     si, msg_dot
        call    print
        jmp     .load_loop

.loaded:
        mov     dl, [boot_drive]        ; hand the boot drive to the kernel
        mov     ebx, [bpb_hidden_secs]  ; and the LBA of this volume
        jmp     KERNEL_SEG:0x0000

; =============================================================================
; read_sector: read one 512-byte sector.
;   EAX = LBA (absolute, on the disk), ES:BX = destination.
;   Preserves all registers.  Retries 3 times, then halts with an error.
; =============================================================================
read_sector:
        pushad
        mov     byte [retries], 3
.retry:
        cmp     byte [use_lba], 0
        je      .chs

        ; ---- LBA read via Disk Address Packet ----
        mov     [dap_lba], eax
        mov     [dap_off], bx
        mov     [dap_seg], es
        mov     si, dap
        mov     ah, 0x42
        mov     dl, [boot_drive]
        int     0x13
        jnc     .ok
        jmp     .fail

.chs:   ; ---- CHS read: convert LBA -> cylinder/head/sector ----
        xor     edx, edx
        movzx   ecx, word [bpb_secs_per_track]
        div     ecx                     ; EAX = LBA / SPT,  EDX = LBA % SPT
        inc     dl                      ; sector numbers are 1-based
        mov     cl, dl                  ; CL = sector (1..63)
        xor     edx, edx
        movzx   ebx, word [bpb_heads]
        div     ebx                     ; EAX = cylinder, EDX = head
        mov     dh, dl                  ; DH = head
        mov     ch, al                  ; CH = cylinder bits 7:0
        shl     ah, 6
        or      cl, ah                  ; CL[7:6] = cylinder bits 9:8
        mov     dl, [boot_drive]
        mov     bp, sp
        mov     bx, [bp+16]             ; original BX from the PUSHAD frame
        mov     ax, 0x0201              ; read 1 sector
        int     0x13
        jnc     .ok

.fail:  ; ---- reset the drive and try again ----
        dec     byte [retries]
        jz      .error
        xor     ah, ah
        mov     dl, [boot_drive]
        int     0x13
        popad
        pushad
        jmp     .retry
.ok:
        popad
        ret
.error:
        mov     si, msg_disk_error
        call    print
        ; fall through to halt

; -----------------------------------------------------------------------------
halt:
        mov     si, msg_reboot
        call    print
        xor     ax, ax
        int     0x16                    ; wait for a key
        int     0x19                    ; and reboot

; -----------------------------------------------------------------------------
; print: write NUL-terminated string at DS:SI using the BIOS teletype service
; -----------------------------------------------------------------------------
print:
        push    ax
        push    bx
.next:  lodsb
        or      al, al
        jz      .done
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .next
.done:  pop     bx
        pop     ax
        ret

; =============================================================================
; Data
; =============================================================================
%ifdef NANODOS
msg_banner:     db 13, 10, "NanoDOS boot", 0
%else
msg_banner:     db 13, 10, "Ember boot", 0
%endif
msg_dot:        db ".", 0
msg_disk_error: db 13, 10, "Disk read error!", 0
msg_reboot:     db 13, 10, "Press any key to reboot.", 0

boot_drive:     db 0
use_lba:        db 0
retries:        db 0
sectors_left:   dw 0

; Disk Address Packet for INT 13h AH=42h
        align 4
dap:
        db 0x10                         ; packet size
        db 0
dap_count:      dw 1                    ; sectors to transfer
dap_off:        dw 0                    ; destination offset
dap_seg:        dw 0                    ; destination segment
dap_lba:        dq 0                    ; starting LBA

; --- pad to 510 bytes and add the boot signature ------------------------------
        times 510-($-$$) db 0
        dw 0xAA55
