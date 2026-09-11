/* =============================================================================
   stub.c - Ember 2.0: the UEFI application that boots a BIOS kernel
   -----------------------------------------------------------------------------
   Ember boots as an MBR disk and talks to the BIOS.  A UEFI-only machine has
   neither, so this does three things and then gets out of the way:

     1. Reads EMBER.IMG - the same disk image the legacy stick is written
        from - off the volume it was launched from, into memory below four
        gigabytes.  That copy is the disk Ember will run on.
     2. Finds what the machine has: the framebuffer, the memory map, and the
        top of conventional memory.  Chooses where the shim goes (as high as
        conventional memory allows) and where the disk goes (as high as the
        contiguous run above a megabyte allows), and tells the shim.
     3. Leaves boot services, and calls the shim.  The shim (shim.asm) drops
        the processor to real mode, becomes the BIOS, and boots the image.

   Everything after step 3 is the shim's.  Everything before it is ordinary
   UEFI, and the tables are the same hand-rolled ones probe.c uses.

   Built by tools/build_ember2.py.  Goes on a FAT32 stick as
   \EFI\BOOT\BOOTX64.EFI, with EMBER.IMG beside it in the root.
   ========================================================================== */

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef unsigned long long  uptr;
typedef u64                 EFI_STATUS;
typedef void               *EFI_HANDLE;

#define EFIAPI  __attribute__((ms_abi))
#define NULLPTR ((void *)0)

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } EFI_GUID;
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
    EFI_STATUS (EFIAPI *AllocatePages)(u32, u32, uptr, u64 *);
    EFI_STATUS (EFIAPI *FreePages)(u64, uptr);
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
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, uptr);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(uptr);
    void *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    u16 *FirmwareVendor;
    u32 FirmwareRevision, pad;
    EFI_HANDLE ConsoleInHandle;
    SIMPLE_TEXT_IN *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    SIMPLE_TEXT_OUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    SIMPLE_TEXT_OUT *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uptr NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

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
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct FILE_PROTOCOL *, uptr *, void *);
    void *Write, *GetPosition, *SetPosition;
    EFI_STATUS (EFIAPI *GetInfo)(struct FILE_PROTOCOL *, EFI_GUID *, uptr *,
                                 void *);
    void *SetInfo, *Flush;
} FILE_PROTOCOL;
typedef struct {
    u64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *, FILE_PROTOCOL **);
} SIMPLE_FS;

typedef struct {
    u32 Type, pad;
    u64 PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} MEMORY_DESCRIPTOR;

static EFI_GUID GUID_GOP =
    {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static EFI_GUID GUID_SIMPLE_FS =
    {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_LOADED_IMAGE =
    {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_FILE_INFO =
    {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

/* the shim, as tools/build_ember2.py wrapped it */
#include "shim_bin.h"

/* the header at the front of it - see shim.asm */
#define SH_SIZE     4
#define SH_ENTRY    8
#define SH_FB_BASE  16
#define SH_FB_W     24
#define SH_FB_H     28
#define SH_FB_PITCH 32
#define SH_FB_BGR   36
#define SH_RD_BASE  40
#define SH_RD_SIZE  44
#define SH_LOW_TOP  48
#define SH_EXT_END  52
#define SH_E820_N   56
#define SH_E820     64
#define E820_MAX    32

#define SHIM_ALLOC  0x10000             /* 64 KB: room for the shim, aligned */
#define EXT_CAP     0x80000000ULL       /* what Ember is known to cope with */

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* ---- saying things, then numbers ----------------------------------------- */
static u16 wide[160];

static void put(const char *s)
{
    int i = 0;
    while (s[i] && i < 158) { wide[i] = (u16)(u8)s[i]; i++; }
    wide[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, wide);
}
static void line(const char *s) { put(s); put("\r\n"); }

static char numbuf[24];
static const char *hex(u64 v)
{
    static const char d[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 16; i++) numbuf[15 - i] = d[(v >> (i * 4)) & 15];
    numbuf[16] = 0;
    i = 0;
    while (i < 15 && numbuf[i] == '0') i++;
    return numbuf + i;
}
static const char *dec(u64 v)
{
    int i = 23;
    numbuf[i] = 0;
    if (!v) numbuf[--i] = '0';
    while (v) { numbuf[--i] = (char)('0' + v % 10); v /= 10; }
    return numbuf + i;
}
static void say_hex(const char *label, u64 v)
{ put(label); put(" "); put(hex(v)); line("h"); }
static void say_dec(const char *label, u64 v, const char *unit)
{ put(label); put(" "); put(dec(v)); line(unit); }

static void fail(const char *why)
{
    EFI_INPUT_KEY k;
    line("");
    put("Ember 2.0 cannot start: ");
    line(why);
    line("Press a key.");
    while (ST->ConIn->ReadKeyStroke(ST->ConIn, &k)) BS->Stall(50000);
}

/* ---- memory helpers; the compiler may call these on its own ------------ */
void *memcpy(void *d, const void *s, uptr n)
{
    u8 *dd = d; const u8 *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, uptr n)
{
    u8 *dd = d;
    while (n--) *dd++ = (u8)c;
    return d;
}

static void put32(u8 *p, u32 off, u32 v) { memcpy(p + off, &v, 4); }
static void put64(u8 *p, u32 off, u64 v) { memcpy(p + off, &v, 8); }
static u32  get32(const u8 *p, u32 off) { u32 v; memcpy(&v, p + off, 4); return v; }

/* ---- the memory map, read fresh each time it is wanted ------------------ */
static u8  *map;
static uptr map_size, map_key, desc_size;

static int read_map(void)
{
    uptr size = 0;
    u32 ver;
    if (map) { BS->FreePool(map); map = NULLPTR; }
    BS->GetMemoryMap(&size, NULLPTR, &map_key, &desc_size, &ver);
    size += desc_size * 8;
    if (BS->AllocatePool(2 /* loader data */, size, (void **)&map)) return 0;
    if (BS->GetMemoryMap(&size, map, &map_key, &desc_size, &ver)) return 0;
    map_size = size;
    return 1;
}

/* memory that is Ember's once boot services are gone: free now, or the
   firmware's only until then */
static int reclaimable(u32 type)
{
    return type == 7 || type == 3 || type == 4 || type == 1;
}

/* the end of the run of reclaimable memory that begins at 'from' */
static u64 run_end(u64 from)
{
    u64 end = from;
    int moved = 1;
    while (moved) {
        uptr off;
        moved = 0;
        for (off = 0; off + desc_size <= map_size; off += desc_size) {
            MEMORY_DESCRIPTOR *d = (MEMORY_DESCRIPTOR *)(map + off);
            u64 s = d->PhysicalStart, e = s + (d->NumberOfPages << 12);
            if (!reclaimable(d->Type)) continue;
            if (s <= end && e > end) { end = e; moved = 1; }
        }
    }
    return end;
}

/* the highest 64 KB-aligned spot below 'limit' that is free (type 7 - it
   has to be allocatable now, not merely reclaimable later) */
static u64 highest_free_slot(u64 limit, u64 need)
{
    u64 best = 0;
    uptr off;
    for (off = 0; off + desc_size <= map_size; off += desc_size) {
        MEMORY_DESCRIPTOR *d = (MEMORY_DESCRIPTOR *)(map + off);
        u64 s = d->PhysicalStart, e = s + (d->NumberOfPages << 12), b;
        if (d->Type != 7) continue;
        if (e > limit) e = limit;
        if (e <= s || e - s < need) continue;
        b = (e - need) & ~0xFFFFULL;
        if (b < s || b < 0x20000) continue;
        if (b > best) best = b;
    }
    return best;
}

/* the first non-reclaimable thing at or above 'from', below a megabyte */
static u64 first_hole_above(u64 from)
{
    u64 hole = 0xA0000;
    uptr off;
    for (off = 0; off + desc_size <= map_size; off += desc_size) {
        MEMORY_DESCRIPTOR *d = (MEMORY_DESCRIPTOR *)(map + off);
        u64 s = d->PhysicalStart;
        if (reclaimable(d->Type)) continue;
        if (s >= from && s < hole) hole = s;
    }
    return hole;
}

/* ---- the disk image ------------------------------------------------------ */
static u64 image_size(FILE_PROTOCOL *f)
{
    u8 info[512];
    uptr n = sizeof info;
    if (f->GetInfo(f, &GUID_FILE_INFO, &n, info)) return 0;
    { u64 v; memcpy(&v, info + 8, 8); return v; }
}

static int read_image(FILE_PROTOCOL *f, u8 *to, u64 size)
{
    u64 done = 0;
    while (done < size) {
        uptr chunk = size - done;
        if (chunk > 0x100000) chunk = 0x100000;
        if (f->Read(f, &chunk, to + done) || !chunk) return 0;
        done += chunk;
    }
    return 1;
}

/* ---- the E820 table Ember and its programs may ask for ------------------ */
static u32 build_e820(u8 *sh, u64 low_top, u64 rd_base, u64 rd_size,
                      u64 shim_base)
{
    u32 n = 0;
    uptr off;
    u64 last_end = 0, last_type = 0;
    /* the first megabyte by hand: what Ember gets, then the rest of it */
    put64(sh + SH_E820 + n * 24, 0, 0);
    put64(sh + SH_E820 + n * 24, 8, low_top);
    put32(sh + SH_E820 + n * 24, 16, 1); n++;
    put64(sh + SH_E820 + n * 24, 0, low_top);
    put64(sh + SH_E820 + n * 24, 8, 0x100000 - low_top);
    put32(sh + SH_E820 + n * 24, 16, 2); n++;
    for (off = 0; off + desc_size <= map_size && n < E820_MAX; off += desc_size) {
        MEMORY_DESCRIPTOR *d = (MEMORY_DESCRIPTOR *)(map + off);
        u64 s = d->PhysicalStart, e = s + (d->NumberOfPages << 12), t;
        if (e <= 0x100000) continue;
        if (s < 0x100000) s = 0x100000;
        if (reclaimable(d->Type) || d->Type == 2) t = 1;
        else if (d->Type == 9) t = 3;           /* ACPI, reclaimable */
        else if (d->Type == 10) t = 4;          /* ACPI, not */
        else t = 2;
        /* our own two allocations are not Ember's */
        if (s < rd_base + rd_size && e > rd_base) t = 2;
        if (s < shim_base + SHIM_ALLOC && e > shim_base) t = 2;
        if (n > 2 && s == last_end && t == last_type) {
            /* extends the previous entry */
            u64 len; memcpy(&len, sh + SH_E820 + (n - 1) * 24 + 8, 8);
            put64(sh + SH_E820 + (n - 1) * 24, 8, len + (e - s));
        } else {
            put64(sh + SH_E820 + n * 24, 0, s);
            put64(sh + SH_E820 + n * 24, 8, e - s);
            put32(sh + SH_E820 + n * 24, 16, (u32)t);
            n++;
        }
        last_end = e; last_type = t;
    }
    return n;
}

/* ========================================================================= */
EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    LOADED_IMAGE *li = NULLPTR;
    SIMPLE_FS *fs = NULLPTR;
    FILE_PROTOCOL *root = NULLPTR, *f = NULLPTR;
    GOP *gop = NULLPTR;
    u64 img_size, run, rd_base = 0, ext_end, shim_base = 0, low_top, hole;
    u8 *sh;
    u32 n;
    int tries;

    ST = st;
    BS = st->BootServices;
    st->ConOut->ClearScreen(st->ConOut);
    line("Ember 2.0");
    line("");

    /* ---- the screen ---- */
    if (BS->LocateProtocol(&GUID_GOP, NULLPTR, (void **)&gop) || !gop
        || !gop->Mode || !gop->Mode->Info) {
        fail("no graphics output protocol - there is nothing to draw on");
        return 1;
    }
    put("screen ");
    put(dec(gop->Mode->Info->HorizontalResolution)); put(" x ");
    put(dec(gop->Mode->Info->VerticalResolution)); put(", framebuffer at ");
    put(hex(gop->Mode->FrameBufferBase)); line("h");
    if (gop->Mode->FrameBufferBase >> 40) {
        fail("the framebuffer is beyond what a 4 MB-page window can map");
        return 1;
    }

    /* ---- the disk image, off the volume we came from ---- */
    if (BS->HandleProtocol(image, &GUID_LOADED_IMAGE, (void **)&li) || !li
        || BS->HandleProtocol(li->DeviceHandle, &GUID_SIMPLE_FS, (void **)&fs)
        || !fs || fs->OpenVolume(fs, &root) || !root) {
        fail("the volume this was started from has no file system");
        return 1;
    }
    if (root->Open(root, &f, (u16 *)L"EMBER.IMG", 1, 0) || !f) {
        fail("EMBER.IMG is not in the root of this volume");
        return 1;
    }
    img_size = image_size(f);
    if (img_size < 0x100000 || img_size > 0x40000000) {
        fail("EMBER.IMG is not a plausible size");
        return 1;
    }
    say_dec("disk image", img_size >> 20, " MB");

    /* ---- where things go ---- */
    if (!read_map()) { fail("the memory map could not be read"); return 1; }
    /* Ember learns how much memory it has from INT 15h E801h, which can only
       describe one run that starts at a megabyte; that run is what it gets.
       Firmware leaves holes - ACPI tables at 8 MB on QEMU's, elsewhere on a
       laptop's - and the run stops at the first.  The disk image need not be
       in it: it goes wherever there is room below four gigabytes, as high as
       possible, and the table below marks it as not Ember's. */
    run = run_end(0x100000);
    if (run > EXT_CAP) run = EXT_CAP;
    say_hex("memory above 1 MB runs to", run);
    rd_base = 0xFFFFFFFF;
    if (BS->AllocatePages(1 /* below this address */, 2 /* loader data */,
                          (img_size + 0xFFF) >> 12, &rd_base)) {
        fail("no room below 4 GB for the disk image");
        return 1;
    }
    ext_end = run;
    if (rd_base >= 0x100000 && rd_base < run) ext_end = rd_base;
    if (ext_end < 0x100000 + 0x100000) {
        fail("less than a megabyte of extended memory - Ember needs more");
        return 1;
    }
    say_hex("disk image at", rd_base);
    say_dec("extended memory for Ember", (ext_end - 0x100000) >> 20, " MB");

    /* the shim: the highest free 64 KB below 640 KB; conventional memory
       ends where it begins, or at the first firmware region above the
       arena, whichever is lower */
    if (!read_map()) { fail("the memory map could not be read"); return 1; }
    shim_base = highest_free_slot(0xA0000, SHIM_ALLOC);
    if (!shim_base) { fail("no free 64 KB below 640 KB for the shim"); return 1; }
    if (BS->AllocatePages(2, 2, SHIM_ALLOC >> 12, &shim_base)) {
        fail("the shim's memory could not be claimed");
        return 1;
    }
    hole = first_hole_above(0x18000);
    low_top = shim_base < hole ? shim_base : hole;
    say_hex("shim at", shim_base);
    say_dec("conventional memory for Ember", low_top >> 10, " KB");
    if (get32(shim_bin, SH_SIZE) > SHIM_ALLOC) {
        fail("the shim has outgrown its 64 KB");
        return 1;
    }

    /* ---- read the image in, place the shim, fill its header ---- */
    if (!read_image(f, (u8 *)(uptr)rd_base, img_size)) {
        fail("EMBER.IMG could not be read");
        return 1;
    }
    f->Close(f);
    root->Close(root);
    sh = (u8 *)(uptr)shim_base;
    memcpy(sh, shim_bin, get32(shim_bin, SH_SIZE));
    put64(sh, SH_FB_BASE, gop->Mode->FrameBufferBase);
    put32(sh, SH_FB_W, gop->Mode->Info->HorizontalResolution);
    put32(sh, SH_FB_H, gop->Mode->Info->VerticalResolution);
    put32(sh, SH_FB_PITCH, gop->Mode->Info->PixelsPerScanLine);
    put32(sh, SH_FB_BGR, gop->Mode->Info->PixelFormat == 1);
    put32(sh, SH_RD_BASE, (u32)rd_base);
    put32(sh, SH_RD_SIZE, (u32)img_size);
    put32(sh, SH_LOW_TOP, (u32)low_top);
    put32(sh, SH_EXT_END, (u32)ext_end);

    /* ---- leave.  The map key must be the current one; allocations above
       changed it, and printing may have too, so read it again right before
       and try twice. ---- */
    line("");
    line("leaving UEFI...");
    for (tries = 0; tries < 4; tries++) {
        if (!read_map()) { fail("the memory map could not be read"); return 1; }
        n = build_e820(sh, low_top, rd_base, img_size, shim_base);
        put32(sh, SH_E820_N, n);
        if (!BS->ExitBootServices(image, map_key)) break;
    }
    if (tries == 4) { fail("the firmware would not let go"); return 1; }

    /* no more firmware.  The shim takes it from here and never returns. */
    ((void (*)(void))(sh + get32(shim_bin, SH_ENTRY)))();
    for (;;) __asm__ volatile ("hlt");
}
