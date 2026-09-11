/* =============================================================================
   probe.c - what a UEFI machine would give Ember, and what it would not
   -----------------------------------------------------------------------------
   Ember boots as an MBR disk and talks to the BIOS: INT 13h for the disk, INT
   10h for the screen, INT 16h for the keyboard.  A UEFI-only machine has none
   of those.  The plan for such a machine is not a port but a stub - a UEFI
   application that takes the machine, drops the processor back to real mode,
   and stands in for the BIOS itself - and whether that plan is worth writing
   depends on four things this program goes and finds out:

     - the screen, which the stub would draw into directly.  That needs the
       graphics output protocol's framebuffer, and its address and shape.
     - the disk, which the stub would read into memory before leaving UEFI
       behind and then serve as a RAM disk.  That needs a block device big
       enough to hold Ember and enough free memory to put it in.
     - the keyboard, which is the one that decides it.  A stub that exits boot
       services has no USB stack; if the built-in keyboard is USB or I2C only,
       the machine will boot to a screen nobody can type at.  ACPI answers
       this: the FADT says whether the machine has an 8042 controller.
     - whether the firmware has a compatibility module after all, which would
       make the whole question moot.

   It changes nothing.  Everything here is a read: no boot entries, no
   variables written, no disk touched.  The report goes on the screen and,
   if the volume it was launched from will take it, into EMBRPROB.TXT beside
   the program - which is easier to send on than a photograph.

   Built by tools/build_uefi.py.  Copy the result onto a FAT32 stick as
   \EFI\BOOT\BOOTX64.EFI and boot it.
   ========================================================================== */

typedef unsigned char       u8;
typedef unsigned short      u16;                /* and CHAR16 */
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef unsigned long long  uptr;               /* UINTN on 64-bit */
typedef u64                 EFI_STATUS;
typedef void               *EFI_HANDLE;

#define EFIAPI  __attribute__((ms_abi))
#define NULLPTR ((void *)0)

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } EFI_GUID;

/* ---- the tables, in the order the specification lays them out ------------ */
typedef struct { u32 sig[2]; u32 rev; u32 size; u32 crc; u32 rsvd; }
        EFI_TABLE_HEADER;

struct SIMPLE_TEXT_OUT;
typedef struct SIMPLE_TEXT_OUT {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct SIMPLE_TEXT_OUT *, u16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(struct SIMPLE_TEXT_OUT *);
    void *SetCursorPosition, *EnableCursor, *Mode;
} SIMPLE_TEXT_OUT;

typedef struct { u16 ScanCode; u16 UnicodeChar; } EFI_INPUT_KEY;
struct SIMPLE_TEXT_IN;
typedef struct SIMPLE_TEXT_IN {
    void *Reset;
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct SIMPLE_TEXT_IN *,
                                       EFI_INPUT_KEY *);
    void *WaitForKey;
} SIMPLE_TEXT_IN;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    void *AllocatePages, *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(uptr *, void *, uptr *, uptr *, u32 *);
    EFI_STATUS (EFIAPI *AllocatePool)(u32, uptr, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify;
    void *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(uptr);
    void *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(u32, EFI_GUID *, void *, uptr *,
                                            EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(u16 *, EFI_GUID *, u32 *, uptr *,
                                     void *);
    void *GetNextVariableName, *SetVariable;
    void *GetNextHighMonotonicCount, *ResetSystem;
    void *UpdateCapsule, *QueryCapsuleCapabilities, *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; }
        EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    u16 *FirmwareVendor;
    u32 FirmwareRevision;
    u32 pad;
    EFI_HANDLE ConsoleInHandle;
    SIMPLE_TEXT_IN *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    SIMPLE_TEXT_OUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    SIMPLE_TEXT_OUT *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uptr NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- the protocols this asks about --------------------------------------- */
typedef struct {
    u32 Version, HorizontalResolution, VerticalResolution, PixelFormat;
    u32 RedMask, GreenMask, BlueMask, ReservedMask;
    u32 PixelsPerScanLine;
} GOP_MODE_INFO;

typedef struct {
    u32 MaxMode, Mode;
    GOP_MODE_INFO *Info;
    uptr SizeOfInfo;
    u64 FrameBufferBase;
    uptr FrameBufferSize;
} GOP_MODE;

typedef struct { void *QueryMode, *SetMode, *Blt; GOP_MODE *Mode; } GOP;

typedef struct {
    u32 MediaId;
    u8 RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
    u8 pad[3];
    u32 BlockSize;
    u32 IoAlign;
    u64 LastBlock;
} BLOCK_IO_MEDIA;

typedef struct {
    u64 Revision;
    BLOCK_IO_MEDIA *Media;
    void *Reset, *ReadBlocks, *WriteBlocks, *FlushBlocks;
} BLOCK_IO;

typedef struct {
    u32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath, *Reserved;
    u32 LoadOptionsSize;
    void *LoadOptions, *ImageBase;
    u64 ImageSize;
} LOADED_IMAGE;

struct FILE_PROTOCOL;
typedef struct FILE_PROTOCOL {
    u64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct FILE_PROTOCOL *, struct FILE_PROTOCOL **,
                              u16 *, u64, u64);
    EFI_STATUS (EFIAPI *Close)(struct FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Delete)(struct FILE_PROTOCOL *);
    void *Read;
    EFI_STATUS (EFIAPI *Write)(struct FILE_PROTOCOL *, uptr *, void *);
    void *GetPosition, *SetPosition, *GetInfo, *SetInfo;
    EFI_STATUS (EFIAPI *Flush)(struct FILE_PROTOCOL *);
} FILE_PROTOCOL;

typedef struct {
    u64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *, FILE_PROTOCOL **);
} SIMPLE_FS;

static EFI_GUID GUID_GOP =
    {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static EFI_GUID GUID_BLOCK_IO =
    {0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_SIMPLE_FS =
    {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_LOADED_IMAGE =
    {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_LEGACY_BIOS =              /* the compatibility module */
    {0x2e3044ac,0x879f,0x490f,{0x97,0x60,0xbb,0xdf,0xaf,0x69,0x5f,0x50}};
static EFI_GUID GUID_ACPI20 =
    {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
static EFI_GUID GUID_ACPI10 =
    {0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID GUID_GLOBAL_VAR =
    {0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}};

static EFI_SYSTEM_TABLE   *ST;
static EFI_BOOT_SERVICES  *BS;

/* =============================================================================
   Saying things
   ========================================================================== */
static char report[16384];
static int  report_len;
static u16  wide[256];

static void put(const char *s)
{
    int i = 0;
    while (s[i] && i < 254) { wide[i] = (u16)(u8)s[i]; i++; }
    wide[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, wide);
    for (i = 0; s[i]; i++)
        if (report_len < (int)sizeof(report) - 1) report[report_len++] = s[i];
}

static void line(const char *s)
{
    put(s);
    /* the console wants both; so does anything that reads the file later */
    ST->ConOut->OutputString(ST->ConOut, (u16 *)L"\r\n");
    if (report_len < (int)sizeof(report) - 2) {
        report[report_len++] = '\r';
        report[report_len++] = '\n';
    }
}

static char numbuf[32];

static const char *hex(u64 v, int digits)
{
    static const char d[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < digits; i++)
        numbuf[digits - 1 - i] = d[(v >> (i * 4)) & 15];
    numbuf[digits] = 0;
    return numbuf;
}

static const char *dec(u64 v)
{
    int i = 31;
    numbuf[i] = 0;
    if (!v) numbuf[--i] = '0';
    while (v) { numbuf[--i] = (char)('0' + (v % 10)); v /= 10; }
    return numbuf + i;
}

/* two values on one line, so the report reads like a table */
static void say(const char *label, const char *value)
{
    int n = 0;
    put(label);
    while (label[n]) n++;
    do { put(" "); n++; } while (n < 34);
    line(value);
}

static void say_dec(const char *label, u64 v) { say(label, dec(v)); }
static void say_hex(const char *label, u64 v, int digits)
{
    char tmp[32];
    const char *h = hex(v, digits);
    int i = 0;
    tmp[i++] = '0'; tmp[i++] = 'x';
    while (*h) tmp[i++] = *h++;
    tmp[i] = 0;
    say(label, tmp);
}
static void say_yn(const char *label, int yes) { say(label, yes ? "yes" : "no"); }

/* =============================================================================
   The machine's own ports and registers
   ========================================================================== */
static u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                              : "a"(leaf), "c"(0));
}

/* =============================================================================
   The four questions
   ========================================================================== */
static void firmware(void)
{
    char v[128];
    int i = 0;
    u16 *fv = ST->FirmwareVendor;
    line("---- the firmware ----");
    while (fv && fv[i] && i < 126) { v[i] = (char)fv[i]; i++; }
    v[i] = 0;
    say("vendor", v);
    say_hex("firmware revision", ST->FirmwareRevision, 8);
    say_hex("UEFI revision", ST->Hdr.rev, 8);
}

static void secure_boot(void)
{
    u8 on = 0, setup = 0;
    uptr size = 1;
    EFI_STATUS s;
    s = ST->RuntimeServices->GetVariable((u16 *)L"SecureBoot", &GUID_GLOBAL_VAR,
                                         NULLPTR, &size, &on);
    if (s) say("secure boot", "the variable is not there");
    else   say_yn("secure boot is on", on != 0);
    size = 1;
    s = ST->RuntimeServices->GetVariable((u16 *)L"SetupMode", &GUID_GLOBAL_VAR,
                                         NULLPTR, &size, &setup);
    if (!s) say_yn("...and it is in setup mode", setup != 0);
}

/* ACPI: the FADT is where the machine says what legacy hardware it still has,
   and it is the only answer to the keyboard question that can be trusted.
   Reading ports to find out would disturb the firmware's own driver; this
   does not touch anything. */
static u32 ids_kbd, ids_mouse, ids_i2c;

/* walk one table for the compressed EISA identifiers above */
static void scan_ids(u8 *t)
{
    u32 len = *(u32 *)(t + 4), i;
    if (len < 36 || len > 0x400000) return;    /* not a table at all */
    for (i = 36; i + 4 <= len; i++) {
        if (t[i] != 0x41 || t[i + 1] != 0xD0) continue;
        if (t[i + 3] == 0x03) ids_kbd++;                /* PNP03xx */
        else if (t[i + 3] == 0x0F && t[i + 2] == 0x13) ids_mouse++;
        else if (t[i + 3] == 0x0C && t[i + 2] == 0x50) ids_i2c++;
    }
}

static void acpi(void)
{
    u8 *rsdp = NULLPTR;
    uptr i;
    u64 xsdt_addr = 0, dsdt = 0;
    u8 *xsdt;
    u32 len, entries, e;
    int fadt_seen = 0;

    line("");
    line("---- what the machine says it still has (ACPI) ----");
    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_GUID *g = &ST->ConfigurationTable[i].VendorGuid;
        if (g->d1 == GUID_ACPI20.d1 && g->d2 == GUID_ACPI20.d2)
            rsdp = (u8 *)ST->ConfigurationTable[i].VendorTable;
        else if (!rsdp && g->d1 == GUID_ACPI10.d1 && g->d2 == GUID_ACPI10.d2)
            rsdp = (u8 *)ST->ConfigurationTable[i].VendorTable;
    }
    if (!rsdp) { say("ACPI", "no table at all"); return; }

    /* the RSDP: revision at 15, the 64-bit XSDT pointer at 24 */
    if (rsdp[15] < 2) { say("ACPI", "version 1 only, no XSDT"); return; }
    for (i = 0; i < 8; i++) xsdt_addr |= ((u64)rsdp[24 + i]) << (i * 8);
    if (!xsdt_addr) { say("ACPI", "the XSDT pointer is empty"); return; }

    xsdt = (u8 *)(uptr)xsdt_addr;
    len = *(u32 *)(xsdt + 4);
    if (len < 36) { say("ACPI", "the XSDT is too short to be one"); return; }
    entries = (len - 36) / 8;
    for (e = 0; e < entries; e++) {
        u64 p = 0;
        u8 *t;
        for (i = 0; i < 8; i++) p |= ((u64)xsdt[36 + e * 8 + i]) << (i * 8);
        t = (u8 *)(uptr)p;
        if (!t) continue;
        if (t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'P') {
            u32 flen = *(u32 *)(t + 4);
            u16 boot_arch;
            say_dec("FADT revision", t[8]);
            if (t[8] < 3 || flen < 111) {
                /* IAPC_BOOT_ARCH arrived with ACPI 2.0, whose FADT is
                   revision 3.  An older table has zeroes where the field
                   would be, and zeroes here read as "no keyboard, no legacy
                   devices" - which is a confident answer to a question this
                   table cannot answer.  Say so instead. */
                say("legacy hardware flags",
                    "this FADT predates them - see the namespace below");
                dsdt = *(u32 *)(t + 40);       /* which any revision has */
                fadt_seen = 1;
                continue;
            }
            boot_arch = (u16)(t[109] | (t[110] << 8));
            say_hex("IAPC_BOOT_ARCH", boot_arch, 4);
            say_yn("  legacy devices present", (boot_arch & 1) != 0);
            say_yn("  8042 controller (FADT's claim)", (boot_arch & 2) != 0);
            say_yn("  VGA hardware present", (boot_arch & 4) == 0);
            say_yn("  CMOS real-time clock present", (boot_arch & 32) == 0);
            /* the DSDT: the 32-bit pointer at 40, or the 64-bit one at 140
               in a table long enough to have it */
            dsdt = *(u32 *)(t + 40);
            if (flen >= 148) {
                u64 x = 0;
                for (i = 0; i < 8; i++) x |= ((u64)t[140 + i]) << (i * 8);
                if (x) dsdt = x;
            }
            fadt_seen = 1;
        }
    }
    if (!fadt_seen) { say("FADT", "not in the XSDT"); return; }

    /* The FADT's flag is a claim, and on modern laptops it is often wrong in
       the pessimistic direction: cleared even though the keyboard is on a
       perfectly good 8042 behind the embedded controller.  What an operating
       system actually trusts is the namespace - a device whose _HID is
       PNP0303 (or another PNP03xx) is a keyboard controller, whatever the
       flag says, and Linux goes on to use it.  Running an AML interpreter to
       find one would be a project; finding the four bytes an EISA identifier
       compresses to is a loop.  PNP0303 is 41 D0 03 03; PNP0F13, the mouse
       beside it, is 41 D0 13 0F; PNP0C50 is a keyboard over I2C, which is the
       other answer and the one a stub could not use. */
    line("  the namespace, for what an OS would actually find:");
    if (dsdt) scan_ids((u8 *)(uptr)dsdt);
    for (e = 0; e < entries; e++) {
        u64 p = 0;
        u8 *t;
        for (i = 0; i < 8; i++) p |= ((u64)xsdt[36 + e * 8 + i]) << (i * 8);
        t = (u8 *)(uptr)p;
        if (t && t[0] == 'S' && t[1] == 'S' && t[2] == 'D' && t[3] == 'T')
            scan_ids(t);
    }
    say_dec("  PNP03xx keyboard controllers", ids_kbd);
    say_dec("  PNP0F13 PS/2 mice", ids_mouse);
    say_dec("  PNP0C50 I2C HID devices", ids_i2c);
    say("  8042 KEYBOARD, all things considered",
        ids_kbd ? "yes, the namespace has one"
        : "NO - nothing in the namespace claims one");
}

static void screen(void)
{
    GOP *g = NULLPTR;
    static const char *fmt[] = { "red-green-blue, 8 bits each",
                                 "blue-green-red, 8 bits each",
                                 "a bit mask of its own",
                                 "no framebuffer (BLT only)" };
    line("");
    line("---- the screen ----");
    if (BS->LocateProtocol(&GUID_GOP, NULLPTR, (void **)&g) || !g || !g->Mode) {
        say("graphics output", "the protocol is not there");
        return;
    }
    say_dec("modes offered", g->Mode->MaxMode);
    if (g->Mode->Info) {
        say_dec("width", g->Mode->Info->HorizontalResolution);
        say_dec("height", g->Mode->Info->VerticalResolution);
        say_dec("pixels per scan line", g->Mode->Info->PixelsPerScanLine);
        say("pixel format", g->Mode->Info->PixelFormat < 4
                            ? fmt[g->Mode->Info->PixelFormat] : "unknown");
    }
    say_hex("framebuffer at", g->Mode->FrameBufferBase, 16);
    say_dec("framebuffer bytes", g->Mode->FrameBufferSize);
    say_yn("  ...and reachable from 32-bit code",
           (g->Mode->FrameBufferBase + g->Mode->FrameBufferSize)
           <= 0xFFFFFFFFULL);
}

static EFI_HANDLE boot_device;

static void disks(void)
{
    EFI_HANDLE *h = NULLPTR;
    uptr count = 0, i;
    line("");
    line("---- the disks ----");
    if (BS->LocateHandleBuffer(2 /* by protocol */, &GUID_BLOCK_IO, NULLPTR,
                               &count, &h) || !count) {
        say("block devices", "none");
        return;
    }
    say_dec("block devices", count);
    for (i = 0; i < count; i++) {
        BLOCK_IO *b = NULLPTR;
        u64 mb;
        char what[96];
        int n = 0;
        const char *s;
        if (BS->HandleProtocol(h[i], &GUID_BLOCK_IO, (void **)&b) || !b
            || !b->Media)
            continue;
        if (!b->Media->MediaPresent) continue;
        mb = ((b->Media->LastBlock + 1) * b->Media->BlockSize) >> 20;
        s = dec(mb);
        while (*s) what[n++] = *s++;
        what[n++] = ' '; what[n++] = 'M'; what[n++] = 'B'; what[n++] = ',';
        what[n++] = ' ';
        s = dec(b->Media->BlockSize);
        while (*s) what[n++] = *s++;
        s = "-byte blocks";
        while (*s) what[n++] = *s++;
        if (b->Media->LogicalPartition) { s = ", a partition";
                                          while (*s) what[n++] = *s++; }
        if (b->Media->RemovableMedia)   { s = ", removable";
                                          while (*s) what[n++] = *s++; }
        if (b->Media->ReadOnly)         { s = ", read only";
                                          while (*s) what[n++] = *s++; }
        what[n] = 0;
        say(h[i] == boot_device ? "  THIS ONE, we booted from it"
            : b->Media->LogicalPartition ? "  volume" : "  whole disk", what);
    }
}

/* The stub would read Ember into memory and serve it as a RAM disk, so what
   matters is whether there is a big enough free block, and whether the first
   megabyte - where a real-mode kernel has to live - is free to be taken. */
static void memory(void)
{
    uptr size = 0, key = 0, dsize = 0;
    u32 dver = 0;
    u8 *map = NULLPTR;
    u64 free_total = 0, biggest = 0, low_free = 0;
    uptr off;

    line("");
    line("---- the memory ----");
    BS->GetMemoryMap(&size, NULLPTR, &key, &dsize, &dver);
    size += dsize * 8;                          /* room for the map to grow */
    if (BS->AllocatePool(4 /* boot services data */, size, (void **)&map)
        || !map) {
        say("memory map", "could not be read");
        return;
    }
    if (BS->GetMemoryMap(&size, map, &key, &dsize, &dver)) {
        say("memory map", "could not be read");
        BS->FreePool(map);
        return;
    }
    for (off = 0; off + dsize <= size; off += dsize) {
        u32 type = *(u32 *)(map + off);
        u64 start = *(u64 *)(map + off + 8);
        u64 pages = *(u64 *)(map + off + 24);
        u64 bytes = pages << 12;
        /* 7 is conventional memory: free for the taking */
        if (type != 7) continue;
        free_total += bytes;
        if (bytes > biggest) biggest = bytes;
        if (start < 0x100000) {
            u64 end = start + bytes;
            if (end > 0x100000) end = 0x100000;
            low_free += end - start;
        }
    }
    say_dec("free memory, MB", free_total >> 20);
    say_dec("largest free block, MB", biggest >> 20);
    say_dec("free below 1 MB, KB", low_free >> 10);

    /* How much is free below a megabyte is the wrong question for a real-mode
       kernel; where the firmware's pieces sit is the right one.  Ember's
       kernel is at 8000h and its programs want everything from there to the
       top of conventional memory in one piece, so a firmware region in the
       middle matters and one at the edge does not.  Memory the firmware only
       uses while it is running - its loader and boot services - is Ember's
       once the stub has told it to go; the rest stays the firmware's for as
       long as the machine is on. */
    {
        static const char *kind[] = {
            "reserved", "loader code", "loader data",
            "boot services code", "boot services data",
            "RUNTIME CODE", "RUNTIME DATA", "free",
            "unusable", "ACPI tables", "ACPI NVS",
            "memory-mapped I/O", "I/O port space", "PAL code", "persistent" };
        u64 low_after = 0;
        line("  below 1 MB, region by region:");
        for (off = 0; off + dsize <= size; off += dsize) {
            u32 type = *(u32 *)(map + off);
            u64 start = *(u64 *)(map + off + 8);
            u64 end = start + (*(u64 *)(map + off + 24) << 12);
            char row[96];
            const char *s;
            int n = 0, reclaim;
            if (start >= 0x100000) continue;
            if (end > 0x100000) end = 0x100000;
            reclaim = type == 7 || (type >= 1 && type <= 4);
            if (reclaim) low_after += end - start;
            row[n++] = ' '; row[n++] = ' '; row[n++] = ' '; row[n++] = ' ';
            s = hex(start, 5); while (*s) row[n++] = *s++;
            row[n++] = '-';
            s = hex(end - 1, 5); while (*s) row[n++] = *s++;
            row[n++] = ' ';
            s = type < 15 ? kind[type] : "unknown";
            while (*s) row[n++] = *s++;
            if (!reclaim) {
                s = "  (never Ember's)";
                while (*s) row[n++] = *s++;
            }
            row[n] = 0;
            line(row);
        }
        say_dec("free below 1 MB once UEFI is gone, KB", low_after >> 10);
    }
    BS->FreePool(map);
}

static void compatibility(void)
{
    void *p = NULLPTR;
    line("");
    line("---- a compatibility module, after all? ----");
    say_yn("legacy BIOS protocol present",
           BS->LocateProtocol(&GUID_LEGACY_BIOS, NULLPTR, &p) == 0 && p);
}

/* Reading the 8042's status port disturbs nothing - it is the one port of the
   pair that has no side effect - but it is only a corroboration.  ACPI above
   is the answer; this says what the hardware looks like from here. */
static void ports_and_cpu(void)
{
    u32 a, b, c, d;
    char brand[52];
    int i, j;
    u8 st64 = inb(0x64);
    line("");
    line("---- ports, read and not written ----");
    say_hex("8042 status port (64h)", st64, 2);
    say(  "  ...FFh here means nothing answers", st64 == 0xFF ? "FFh" : "not FFh");
    say_hex("interrupt controller mask (21h)", inb(0x21), 2);
    say_hex("second controller mask (A1h)", inb(0xA1), 2);

    line("");
    line("---- the processor ----");
    cpuid(0, &a, &b, &c, &d);
    brand[0] = 0;
    for (i = 0; i < 4; i++) brand[i]     = (char)(b >> (i * 8));
    for (i = 0; i < 4; i++) brand[4 + i] = (char)(d >> (i * 8));
    for (i = 0; i < 4; i++) brand[8 + i] = (char)(c >> (i * 8));
    brand[12] = 0;
    say("vendor", brand);
    cpuid(1, &a, &b, &c, &d);
    say_hex("family/model/stepping", a, 8);
    say_yn("virtual-8086 extensions (VME)", (d & 2) != 0);
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        u32 *w = (u32 *)brand;
        for (j = 0; j < 3; j++) {
            cpuid(0x80000002 + j, &a, &b, &c, &d);
            *w++ = a; *w++ = b; *w++ = c; *w++ = d;
        }
        brand[48] = 0;
        say("model", brand);
    }
}

/* =============================================================================
   Keeping it
   ========================================================================== */
static void save(EFI_HANDLE image)
{
    LOADED_IMAGE *li = NULLPTR;
    SIMPLE_FS *fs = NULLPTR;
    FILE_PROTOCOL *root = NULLPTR, *f = NULLPTR;
    uptr n = (uptr)report_len;

    line("");
    if (BS->HandleProtocol(image, &GUID_LOADED_IMAGE, (void **)&li) || !li) {
        line("(the report could not be saved: no loaded-image protocol)");
        return;
    }
    if (BS->HandleProtocol(li->DeviceHandle, &GUID_SIMPLE_FS, (void **)&fs)
        || !fs) {
        line("(the report could not be saved: the volume has no file system)");
        return;
    }
    if (fs->OpenVolume(fs, &root) || !root) {
        line("(the report could not be saved: the volume would not open)");
        return;
    }
    /* Opening an existing file for writing does not shorten it, so a report
       shorter than the last one left the last one's tail showing through -
       the Yoga's processor turned up at the bottom of the ThinkPad's.  A
       previous report is deleted first; Delete closes the handle itself. */
    if (!root->Open(root, &f, (u16 *)L"EMBRPROB.TXT", 2 | 1, 0) && f)
        f->Delete(f);
    f = NULLPTR;
    /* create, read and write */
    if (root->Open(root, &f, (u16 *)L"EMBRPROB.TXT",
                   0x8000000000000000ULL | 2 | 1, 0) || !f) {
        line("(the report could not be saved: the file would not open)");
        root->Close(root);
        return;
    }
    f->Write(f, &n, report);
    f->Flush(f);
    f->Close(f);
    root->Close(root);
    line("The same thing is in EMBRPROB.TXT on this volume.");
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_INPUT_KEY k;
    ST = st;
    BS = st->BootServices;
    st->ConOut->ClearScreen(st->ConOut);
    line("Ember: what this machine would give a real-mode kernel");
    line("Nothing here is written.  Every line is something read.");
    line("");
    {   /* which volume we came off: the one a stub would read Ember from */
        LOADED_IMAGE *li = NULLPTR;
        if (!BS->HandleProtocol(image, &GUID_LOADED_IMAGE, (void **)&li) && li)
            boot_device = li->DeviceHandle;
    }
    firmware();
    secure_boot();
    acpi();
    screen();
    disks();
    memory();
    compatibility();
    ports_and_cpu();
    save(image);
    line("");
    line("Press a key to leave.");
    while (st->ConIn->ReadKeyStroke(st->ConIn, &k))
        BS->Stall(50000);
    return 0;
}
