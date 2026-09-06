/* touch.c - the touchpad, spoken to over I2C.
 *
 * On a laptop the touchpad is a "HID over I2C" device behind one of the
 * chipset's Serial IO controllers, which are DesignWare I2C blocks at
 * fixed memory addresses.  The firmware sets those up for Windows but
 * leaves them asleep when it boots us, so the driver has to find the
 * controller, wake it, and drive the bus itself.  None of that involves
 * the firmware's mouse impersonation, which is the point.
 *
 * What comes back, in the pad's default mode, is a plain mouse report:
 * six bytes, [length lo][length hi][report 1][buttons][dx][dy].  A read
 * is issued every few milliseconds and collected on a later pass through
 * the main loop, so nothing here ever waits on the bus.
 *
 * The addresses below are what the Yoga 3 Pro told us (see
 * docs/HARDWARE.md); another laptop will differ, and then the driver
 * simply finds nothing and the desktop carries on with whatever mouse it
 * has.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "touch.h"

/* the DesignWare I2C block */
#define IC_CON           0x00
#define IC_TAR           0x04
#define IC_DATA_CMD      0x10
#define IC_SS_SCL_HCNT   0x14
#define IC_SS_SCL_LCNT   0x18
#define IC_FS_SCL_HCNT   0x1C
#define IC_FS_SCL_LCNT   0x20
#define IC_INTR_MASK     0x30
#define IC_RAW_INTR_STAT 0x34
#define IC_RX_TL         0x38
#define IC_TX_TL         0x3C
#define IC_CLR_INTR      0x40
#define IC_CLR_TX_ABRT   0x54
#define IC_ENABLE        0x6C
#define IC_STATUS        0x70
#define IC_TXFLR         0x74
#define IC_RXFLR         0x78
#define IC_SDA_HOLD      0x7C
#define IC_TX_ABRT_SRC   0x80
#define IC_ENABLE_STATUS 0x9C
#define IC_COMP_TYPE     0xFC
#define DW_IDENT         0x44570140u

/* Intel's private registers behind the block, and the copy of the
   device's PCI configuration space on the page after it */
#define PRV_CLOCK        0x800
#define PRV_RESETS       0x804
#define CFG_COMMAND      0x1004
#define CFG_BAR0         0x10           /* within the page itself */
#define CFG_POWER        0x1084

#define CMD_READ         0x100
#define CMD_STOP         0x200
#define CMD_RESTART      0x400

/* where to look, and what for */
#define SWEEP_FROM       0xFE000000u
#define SWEEP_TO         0xFE400000u
#define PAD_ADDRESS      0x2C           /* the Synaptics pad's I2C address */
#define PAD_DESC_REG     0x0020         /* where its HID descriptor is read */
#define REPORT_BYTES     8              /* what a poll asks for */
#define POLL_MS          6
#define SCALE            2              /* counts to pixels */

int touch_present;

static volatile uint32_t *ic;
static uint16_t input_reg, max_input, cmd_reg, vendor, product;
static int busy;                        /* a read is on the bus */
static unsigned issued_ms, last_issue_ms;
static int last_buttons;

static uint32_t rd(int off)            { return ic[off >> 2]; }
static void     wr(int off, uint32_t v) { ic[off >> 2] = v; }

static void delay(unsigned ms)
{
    unsigned t0 = now_ms();
    while (now_ms() - t0 < ms) ;
}

/* ------------------------------------------------------------ the host */
/* The controllers' identity codes: Wildcat Point-LP and Lynxpoint-LP, the
   two chipsets that hide their Serial IO this way.  Host 0 carries the
   touchpad on the machines we know. */
static const uint32_t host0_ids[] = { 0x9CE18086u, 0x9C618086u, 0 };

static int find_host(void)
{
    uint32_t page;
    for (page = SWEEP_FROM; page < SWEEP_TO; page += 0x1000) {
        volatile uint32_t *p = (volatile uint32_t *)page;
        uint32_t id = p[0];
        int i;
        if (id == 0xFFFFFFFFu || id == 0) continue;
        for (i = 0; host0_ids[i]; i++)
            if (id == host0_ids[i]) {
                uint32_t bar0 = p[CFG_BAR0 >> 2] & 0xFFFFF000u; /* p is the page */
                if (bar0 == 0 || bar0 == 0xFFFFF000u) return -1;
                ic = (volatile uint32_t *)bar0;
                return 0;
            }
    }
    return -1;
}

static void host_disable(void)
{
    int n = 100000;
    wr(IC_ENABLE, 0);
    while (n-- && (rd(IC_ENABLE_STATUS) & 1)) ;
}

static void host_enable(void)
{
    int n = 100000;
    wr(IC_ENABLE, 1);
    while (n-- && !(rd(IC_ENABLE_STATUS) & 1)) ;
}

/* what the firmware's own power-on method does, then the block's reset
   and clock */
static int wake_host(void)
{
    wr(CFG_POWER, rd(CFG_POWER) & ~3u);
    delay(5);
    wr(CFG_COMMAND, rd(CFG_COMMAND) | 6);
    wr(PRV_RESETS, 3);
    wr(PRV_CLOCK, rd(PRV_CLOCK) | 1);
    delay(10);
    return rd(IC_COMP_TYPE) == DW_IDENT ? 0 : -1;
}

/* master only, restarts allowed, no interrupts (we poll).  The counts are
   for a 100 MHz clock; fast mode is 400 kHz, standard 100 kHz. */
static void setup_host(int fast)
{
    host_disable();
    wr(IC_CON, fast ? 0x65 : 0x63);
    wr(IC_SS_SCL_HCNT, 0x1AB);
    wr(IC_SS_SCL_LCNT, 0x1F3);
    wr(IC_FS_SCL_HCNT, 0x57);
    wr(IC_FS_SCL_LCNT, 0x9F);
    wr(IC_SDA_HOLD, 0x1E);
    wr(IC_RX_TL, 0);
    wr(IC_TX_TL, 0);
    wr(IC_INTR_MASK, 0);
    wr(IC_TAR, PAD_ADDRESS);
    host_enable();
}

/* ------------------------------------------------------------ transfers */
/* A whole transfer, waited for: only used while setting up.  Writes wn
   bytes, then reads rn with a restart between.  Returns -1 on an abort or
   a timeout. */
static int xfer(const uint8_t *w, int wn, uint8_t *r, int rn)
{
    int total = wn + rn, sent = 0, got = 0;
    unsigned t0 = now_ms();

    rd(IC_CLR_INTR);
    rd(IC_CLR_TX_ABRT);
    for (;;) {
        if (rd(IC_RAW_INTR_STAT) & (1 << 6)) {          /* aborted */
            rd(IC_CLR_TX_ABRT);
            host_disable();
            host_enable();
            return -1;
        }
        if (sent < total && rd(IC_TXFLR) < 8) {
            uint32_t cmd;
            if (sent < wn) cmd = w[sent];
            else {
                cmd = CMD_READ;
                if (sent == wn && wn) cmd |= CMD_RESTART;
            }
            if (sent == total - 1) cmd |= CMD_STOP;
            wr(IC_DATA_CMD, cmd);
            sent++;
        }
        if (rd(IC_RXFLR)) {
            uint8_t b = (uint8_t)rd(IC_DATA_CMD);
            if (got < rn) r[got++] = b;
        }
        if (sent == total && got == rn && !(rd(IC_STATUS) & 1))
            return 0;
        if (now_ms() - t0 > 100) {
            host_disable();
            host_enable();
            return -1;
        }
    }
}

static int hid_command(uint8_t opcode, uint8_t arg)
{
    uint8_t w[4];
    w[0] = (uint8_t)cmd_reg;
    w[1] = (uint8_t)(cmd_reg >> 8);
    w[2] = arg;
    w[3] = opcode;
    return xfer(w, 4, 0, 0);
}

/* ------------------------------------------------------------ the driver */
int touch_open(void)
{
    uint8_t w[2], desc[30];
    int fast = 1;

    touch_present = 0;
    if (find_host() != 0) {
        sys_log("touchpad: no I2C host found");
        return -1;
    }
    if (wake_host() != 0) {
        sys_logf("touchpad: host at %08X would not wake (%08X)",
                 (unsigned)ic, rd(IC_COMP_TYPE));
        return -1;
    }

    /* the HID descriptor: thirty bytes that say where everything else is */
    w[0] = PAD_DESC_REG & 0xFF;
    w[1] = PAD_DESC_REG >> 8;
    setup_host(1);
    if (xfer(w, 2, desc, 30) != 0 || desc[0] != 0x1E || desc[1] != 0) {
        fast = 0;                               /* try it slower */
        setup_host(0);
        if (xfer(w, 2, desc, 30) != 0 || desc[0] != 0x1E || desc[1] != 0) {
            sys_logf("touchpad: host at %08X, but no HID descriptor at %02X",
                     (unsigned)ic, PAD_ADDRESS);
            return -1;
        }
    }
    input_reg = desc[8] | (desc[9] << 8);
    max_input = desc[10] | (desc[11] << 8);
    cmd_reg   = desc[16] | (desc[17] << 8);
    vendor    = desc[20] | (desc[21] << 8);
    product   = desc[22] | (desc[23] << 8);
    (void)input_reg;

    /* on, and reset: the reset answers with an empty report */
    hid_command(0x08, 0x00);                    /* SET_POWER, on */
    delay(5);
    hid_command(0x01, 0x00);                    /* RESET */
    delay(100);
    xfer(0, 0, desc, 2);

    sys_logf("touchpad: %04X:%04X on host %08X, %s mode, input up to %u bytes",
             vendor, product, (unsigned)ic, fast ? "fast" : "standard", max_input);
    touch_present = 1;
    busy = 0;
    last_issue_ms = now_ms();

    /* Poll often enough to feel immediate: the main loop sleeps until an
       interrupt when idle, so the timer is asked for 200 a second. */
    outb(0x43, 0x36);
    outb(0x40, 5966 & 0xFF);
    outb(0x40, 5966 >> 8);
    return 0;
}

void touch_close(void)
{
    if (!touch_present) return;
    touch_present = 0;
    outb(0x43, 0x36);                           /* the timer back to 18.2 Hz */
    outb(0x40, 0);
    outb(0x40, 0);
}

/* Called from the main loop: start a read, or collect the one in flight.
   The controller does the waiting; we only look in on it. */
void touch_poll(void)
{
    unsigned now;
    if (!touch_present) return;
    now = now_ms();

    if (busy) {
        uint8_t b[REPORT_BYTES];
        int i, len;
        if (rd(IC_RAW_INTR_STAT) & (1 << 6)) {  /* the pad did not answer */
            rd(IC_CLR_TX_ABRT);
            host_disable();
            host_enable();
            busy = 0;
            return;
        }
        if ((int)rd(IC_RXFLR) < REPORT_BYTES) {
            if (now - issued_ms > 40) {         /* stuck: start afresh */
                host_disable();
                host_enable();
                busy = 0;
            }
            return;
        }
        for (i = 0; i < REPORT_BYTES; i++)
            b[i] = (uint8_t)rd(IC_DATA_CMD);
        busy = 0;

        len = b[0] | (b[1] << 8);
        if (len >= 6 && len <= 64 && b[2] == 1) {
            int buttons = b[3] & 3;
            int dx = (int8_t)b[4], dy = (int8_t)b[5];
            if (dx || dy || buttons != last_buttons)
                input_inject_mouse(dx * SCALE, dy * SCALE, buttons);
            last_buttons = buttons;
        }
        return;
    }

    if (now - last_issue_ms < POLL_MS) return;
    {
        int i;
        for (i = 0; i < REPORT_BYTES; i++)
            wr(IC_DATA_CMD, CMD_READ | (i == REPORT_BYTES - 1 ? CMD_STOP : 0));
    }
    busy = 1;
    issued_ms = last_issue_ms = now;
}
