/* touch.c - the touchpad and the touchscreen, spoken to over I2C.
 *
 * On a laptop both are "HID over I2C" devices behind the chipset's Serial
 * IO controllers, which are DesignWare I2C blocks at fixed memory
 * addresses.  The firmware sets those up for Windows but leaves them
 * asleep when it boots us, so the driver has to find each controller,
 * wake it, and drive the bus itself.  None of that involves the
 * firmware's mouse impersonation, which is the point.
 *
 * Each device describes its own reports in a HID report descriptor, and
 * a small reader of those tells us where X, Y, the buttons and the
 * finger-down flag sit in a report and how big X and Y can get.  The pad
 * reports movement (a mouse report); the screen reports a position, and
 * a finger on it is a press.
 *
 * A read is issued every few milliseconds and collected on a later pass
 * through the main loop, so nothing here ever waits on the bus once it
 * is running.  The addresses below are what the Yoga 3 Pro told us (see
 * docs/HARDWARE.md); on another machine the driver finds nothing and the
 * desktop carries on with whatever mouse it has.
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
#define IC_ENABLE_STATUS 0x9C
#define IC_COMP_TYPE     0xFC
#define DW_IDENT         0x44570140u

/* Intel's private registers behind the block, and the copy of the
   device's PCI configuration space on the page after it */
#define PRV_CLOCK        0x800
#define PRV_RESETS       0x804
#define CFG_COMMAND      0x1004
#define CFG_BAR0         0x10           /* within the configuration page */
#define CFG_POWER        0x1084

#define CMD_READ         0x100
#define CMD_STOP         0x200
#define CMD_RESTART      0x400

#define SWEEP_FROM       0xFE000000u
#define SWEEP_TO         0xFE400000u
#define POLL_MS          4
#define PAD_SCALE        2              /* pad counts to pixels */

int touch_present;

/* one device: where it is and what its reports look like */
struct hid_dev {
    const char *name;
    uint32_t host_id;                   /* the controller's PCI identity */
    int address, desc_reg;              /* on the bus, and where its descriptor is */
    int absolute;                       /* a screen (positions), not a pad (movement) */

    volatile uint32_t *ic;
    int present, busy;
    unsigned issued_ms, last_issue_ms;
    uint16_t input_reg, max_input, cmd_reg, rdesc_reg, rdesc_len, vendor, product;
    int read_n;                         /* bytes asked for per poll */

    /* from the report descriptor: bit offsets within the report body
       (after the report id), sizes, and the largest X and Y */
    int report_id;
    int x_off, x_bits, y_off, y_bits, x_max, y_max;
    int btn_off, btn_bits, tip_off;
    int last_buttons, was_down;
};

static struct hid_dev devs[] = {
    { "touchpad",    0x9CE18086u, 0x2C, 0x0020, 0 },
    { "touchscreen", 0x9CE28086u, 0x4A, 0x0000, 1 },
};
#define NDEV ((int)(sizeof devs / sizeof devs[0]))

static uint32_t rd(struct hid_dev *d, int off)             { return d->ic[off >> 2]; }
static void     wr(struct hid_dev *d, int off, uint32_t v) { d->ic[off >> 2] = v; }

static void delay(unsigned ms)
{
    unsigned t0 = now_ms();
    while (now_ms() - t0 < ms) ;
}

/* ------------------------------------------------------------ the host */
static int find_host(struct hid_dev *d)
{
    uint32_t page;
    for (page = SWEEP_FROM; page < SWEEP_TO; page += 0x1000) {
        volatile uint32_t *p = (volatile uint32_t *)page;
        if (p[0] == d->host_id) {
            uint32_t bar0 = p[CFG_BAR0 >> 2] & 0xFFFFF000u;
            if (bar0 == 0 || bar0 == 0xFFFFF000u) return -1;
            d->ic = (volatile uint32_t *)bar0;
            return 0;
        }
    }
    return -1;
}

static void host_disable(struct hid_dev *d)
{
    int n = 100000;
    wr(d, IC_ENABLE, 0);
    while (n-- && (rd(d, IC_ENABLE_STATUS) & 1)) ;
}

static void host_enable(struct hid_dev *d)
{
    int n = 100000;
    wr(d, IC_ENABLE, 1);
    while (n-- && !(rd(d, IC_ENABLE_STATUS) & 1)) ;
}

/* what the firmware's own power-on method does, then the block's reset
   and clock */
static int wake_host(struct hid_dev *d)
{
    wr(d, CFG_POWER, rd(d, CFG_POWER) & ~3u);
    delay(5);
    wr(d, CFG_COMMAND, rd(d, CFG_COMMAND) | 6);
    wr(d, PRV_RESETS, 3);
    wr(d, PRV_CLOCK, rd(d, PRV_CLOCK) | 1);
    delay(10);
    return rd(d, IC_COMP_TYPE) == DW_IDENT ? 0 : -1;
}

/* master only, restarts allowed, no interrupts (we poll).  The counts are
   for a 100 MHz clock; fast mode is 400 kHz, standard 100 kHz. */
static void setup_host(struct hid_dev *d, int fast)
{
    host_disable(d);
    wr(d, IC_CON, fast ? 0x65 : 0x63);
    wr(d, IC_SS_SCL_HCNT, 0x1AB);
    wr(d, IC_SS_SCL_LCNT, 0x1F3);
    wr(d, IC_FS_SCL_HCNT, 0x57);
    wr(d, IC_FS_SCL_LCNT, 0x9F);
    wr(d, IC_SDA_HOLD, 0x1E);
    wr(d, IC_RX_TL, 0);
    wr(d, IC_TX_TL, 0);
    wr(d, IC_INTR_MASK, 0);
    wr(d, IC_TAR, d->address);
    host_enable(d);
}

/* ------------------------------------------------------------ transfers */
/* A whole transfer, waited for: only used while setting up.  Writes wn
   bytes, then reads rn with a restart between.  -1 on an abort or a
   timeout. */
static int xfer(struct hid_dev *d, const uint8_t *w, int wn, uint8_t *r, int rn)
{
    int total = wn + rn, sent = 0, got = 0;
    unsigned t0 = now_ms();

    rd(d, IC_CLR_INTR);
    rd(d, IC_CLR_TX_ABRT);
    for (;;) {
        if (rd(d, IC_RAW_INTR_STAT) & (1 << 6)) {       /* aborted */
            rd(d, IC_CLR_TX_ABRT);
            host_disable(d);
            host_enable(d);
            return -1;
        }
        if (sent < total && rd(d, IC_TXFLR) < 8) {
            uint32_t cmd;
            if (sent < wn) cmd = w[sent];
            else {
                cmd = CMD_READ;
                if (sent == wn && wn) cmd |= CMD_RESTART;
            }
            if (sent == total - 1) cmd |= CMD_STOP;
            wr(d, IC_DATA_CMD, cmd);
            sent++;
        }
        if (rd(d, IC_RXFLR)) {
            uint8_t b = (uint8_t)rd(d, IC_DATA_CMD);
            if (got < rn) r[got++] = b;
        }
        if (sent == total && got == rn && !(rd(d, IC_STATUS) & 1))
            return 0;
        if (now_ms() - t0 > 200) {
            host_disable(d);
            host_enable(d);
            return -1;
        }
    }
}

static int read_register(struct hid_dev *d, int reg, uint8_t *out, int n)
{
    uint8_t w[2];
    w[0] = (uint8_t)reg;
    w[1] = (uint8_t)(reg >> 8);
    return xfer(d, w, 2, out, n);
}

static int hid_command(struct hid_dev *d, uint8_t opcode, uint8_t arg)
{
    uint8_t w[4];
    w[0] = (uint8_t)d->cmd_reg;
    w[1] = (uint8_t)(d->cmd_reg >> 8);
    w[2] = arg;
    w[3] = opcode;
    return xfer(d, w, 4, 0, 0);
}

/* ------------------------------------------------------------ the descriptor */
/* Enough of a HID report descriptor reader to find X, Y, the buttons and
   the finger flag in the first input report that carries X.  Items are a
   prefix byte (tag, type, size) and a little-endian value; a Main "Input"
   item lays down the usages collected since the last one, report_count
   fields of report_size bits each. */
static void read_layout(struct hid_dev *d, const uint8_t *p, int n)
{
    int i = 0, page = 0, lmax = 0, rsize = 0, rcount = 0, rid = 0, bits = 0;
    int usages[32], nusage = 0, umin = 0, done = 0, btn_rid = -1, tip_rid = -1;
    d->x_bits = 0;
    d->y_bits = 0;
    d->btn_bits = 0;
    d->tip_off = -1;
    d->report_id = 0;
    while (i < n && !done) {
        int prefix = p[i], size = prefix & 3, type = (prefix >> 2) & 3, tag = prefix >> 4;
        int value = 0, k;
        if (prefix == 0xFE) {                   /* long item: skip */
            int len = i + 1 < n ? p[i + 1] : 0;
            i += 3 + len;
            continue;
        }
        if (size == 3) size = 4;
        for (k = 0; k < size && i + 1 + k < n; k++)
            value |= p[i + 1 + k] << (8 * k);
        i += 1 + size;
        if (type == 1) {                        /* global */
            if (tag == 0) page = value;
            else if (tag == 2) lmax = value;
            else if (tag == 7) rsize = value;
            else if (tag == 9) rcount = value;
            else if (tag == 8) { rid = value; bits = 0; nusage = 0; }
        } else if (type == 2) {                 /* local */
            if (tag == 0 && nusage < 32)
                usages[nusage++] = (size >= 4 ? value : (page << 16) | value);
            else if (tag == 1) umin = value;    /* a range of usages: buttons 1..3 */
            else if (tag == 2) {
                int u;
                for (u = umin; u <= value && nusage < 32; u++)
                    usages[nusage++] = (page << 16) | u;
            }
        } else if (type == 0) {                 /* main */
            if (tag == 8) {                     /* input */
                int f;
                for (f = 0; f < rcount; f++) {
                    int u = nusage ? usages[f < nusage ? f : nusage - 1] : 0;
                    int off = bits + f * rsize;
                    if (u == 0x00010030 && !d->x_bits) {         /* X */
                        d->x_off = off; d->x_bits = rsize; d->x_max = lmax;
                        d->report_id = rid;
                    } else if (u == 0x00010031 && !d->y_bits) {  /* Y */
                        d->y_off = off; d->y_bits = rsize; d->y_max = lmax;
                    } else if ((u >> 16) == 0x0009 && !d->btn_bits) {   /* buttons */
                        d->btn_off = off; d->btn_bits = rsize * rcount; btn_rid = rid;
                    } else if (u == 0x000D0042 && d->tip_off < 0) {      /* tip switch */
                        d->tip_off = off; tip_rid = rid;
                    }
                }
                bits += rsize * rcount;
            }
            nusage = 0;
            if (tag == 12 && d->x_bits && d->y_bits)    /* the collection with X is done */
                done = 1;
        }
    }
    /* buttons or a finger flag count only if they are in the same report as X */
    if (btn_rid != d->report_id) d->btn_bits = 0;
    if (tip_rid != d->report_id) d->tip_off = -1;
}

static int field(const uint8_t *body, int off, int bits)
{
    int v = 0, k;
    for (k = 0; k < bits; k++) {
        int b = off + k;
        if (body[b >> 3] & (1 << (b & 7))) v |= 1 << k;
    }
    return v;
}

/* ------------------------------------------------------------ the driver */
static int open_device(struct hid_dev *d)
{
    uint8_t desc[30], rdesc[256], ack[2];
    int fast = 1;

    d->present = 0;
    if (find_host(d) != 0) {
        sys_logf("%s: no I2C host %08X found", d->name, d->host_id);
        return -1;
    }
    if (wake_host(d) != 0) {
        sys_logf("%s: host at %08X would not wake (%08X)", d->name,
                 (unsigned)d->ic, rd(d, IC_COMP_TYPE));
        return -1;
    }

    /* the HID descriptor: thirty bytes that say where everything else is */
    setup_host(d, 1);
    if (read_register(d, d->desc_reg, desc, 30) != 0 || desc[0] != 0x1E || desc[1] != 0) {
        fast = 0;
        setup_host(d, 0);
        if (read_register(d, d->desc_reg, desc, 30) != 0 || desc[0] != 0x1E || desc[1] != 0) {
            sys_logf("%s: host at %08X, but no HID descriptor at %02X", d->name,
                     (unsigned)d->ic, d->address);
            return -1;
        }
    }
    d->rdesc_len = desc[4] | (desc[5] << 8);
    d->rdesc_reg = desc[6] | (desc[7] << 8);
    d->input_reg = desc[8] | (desc[9] << 8);
    d->max_input = desc[10] | (desc[11] << 8);
    d->cmd_reg   = desc[16] | (desc[17] << 8);
    d->vendor    = desc[20] | (desc[21] << 8);
    d->product   = desc[22] | (desc[23] << 8);

    /* the report descriptor, for where things are in a report */
    if (d->rdesc_len > sizeof rdesc) d->rdesc_len = sizeof rdesc;
    if (read_register(d, d->rdesc_reg, rdesc, d->rdesc_len) != 0) {
        sys_logf("%s: could not read its report descriptor", d->name);
        return -1;
    }
    read_layout(d, rdesc, d->rdesc_len);
    if (!d->x_bits || !d->y_bits) {
        sys_logf("%s: no X and Y in its report descriptor", d->name);
        return -1;
    }

    /* on, and reset: the reset answers with an empty report */
    hid_command(d, 0x08, 0x00);                 /* SET_POWER, on */
    delay(5);
    hid_command(d, 0x01, 0x00);                 /* RESET */
    delay(100);
    xfer(d, 0, 0, ack, 2);

    d->read_n = d->max_input;
    if (d->read_n > 24) d->read_n = 24;         /* the FIFO holds 32 commands */
    if (d->read_n < 8) d->read_n = 8;
    sys_logf("%s: %04X:%04X on host %08X, %s mode; report %d: X %d bits at %d up to %d, "
             "Y %d bits at %d up to %d, %s at %d",
             d->name, d->vendor, d->product, (unsigned)d->ic, fast ? "fast" : "standard",
             d->report_id, d->x_bits, d->x_off, d->x_max, d->y_bits, d->y_off, d->y_max,
             d->absolute ? "finger" : "buttons",
             d->absolute ? d->tip_off : d->btn_off);
    d->present = 1;
    d->busy = 0;
    d->last_issue_ms = now_ms();
    return 0;
}

int touch_open(void)
{
    int i, any = 0;
    for (i = 0; i < NDEV; i++)
        if (open_device(&devs[i]) == 0) any = 1;
    touch_present = any;
    if (!any) return -1;

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

/* a report has arrived: make it movement or a position */
static void report(struct hid_dev *d, const uint8_t *b, int n)
{
    int len = b[0] | (b[1] << 8);
    const uint8_t *body = b + 3;                /* after the length and the id */
    if (len < 3 || len > n || b[2] != d->report_id) return;

    if (!d->absolute) {
        int buttons = d->btn_bits ? field(body, d->btn_off, d->btn_bits) & 3 : 0;
        int dx = field(body, d->x_off, d->x_bits), dy = field(body, d->y_off, d->y_bits);
        if (d->x_bits == 8) { dx = (int8_t)dx; dy = (int8_t)dy; }
        else if (d->x_bits == 16) { dx = (int16_t)dx; dy = (int16_t)dy; }
        if (dx || dy || buttons != d->last_buttons)
            input_inject_mouse(dx * PAD_SCALE, dy * PAD_SCALE, buttons);
        d->last_buttons = buttons;
    } else {
        int down = d->tip_off >= 0 ? field(body, d->tip_off, 1) : 1;
        int x = field(body, d->x_off, d->x_bits), y = field(body, d->y_off, d->y_bits);
        if (down) {
            int sx = d->x_max ? (int)((long)x * (mouse_max_x + 1) / (d->x_max + 1)) : x;
            int sy = d->y_max ? (int)((long)y * (mouse_max_y + 1) / (d->y_max + 1)) : y;
            input_inject_absolute(sx, sy, 1);
            d->was_down = 1;
        } else if (d->was_down) {
            input_inject_absolute(mouse_x, mouse_y, 0);
            d->was_down = 0;
        }
    }
}

/* Called from the main loop: start a read, or collect the one in flight.
   The controller does the waiting; we only look in on it. */
static void poll_device(struct hid_dev *d)
{
    unsigned now = now_ms();
    if (!d->present) return;

    if (d->busy) {
        uint8_t b[32];
        int i;
        if (rd(d, IC_RAW_INTR_STAT) & (1 << 6)) {       /* it did not answer */
            rd(d, IC_CLR_TX_ABRT);
            host_disable(d);
            host_enable(d);
            d->busy = 0;
            return;
        }
        if ((int)rd(d, IC_RXFLR) < d->read_n) {
            if (now - d->issued_ms > 40) {              /* stuck: start afresh */
                host_disable(d);
                host_enable(d);
                d->busy = 0;
            }
            return;
        }
        for (i = 0; i < d->read_n; i++)
            b[i] = (uint8_t)rd(d, IC_DATA_CMD);
        d->busy = 0;
        report(d, b, d->read_n);
        /* and the next read goes out now: the loop sleeps between passes,
           so waiting for another pass to issue it would halve the rate */
        if (now - d->last_issue_ms < POLL_MS) return;
    }

    if (now - d->last_issue_ms < POLL_MS) return;
    {
        int i;
        for (i = 0; i < d->read_n; i++)
            wr(d, IC_DATA_CMD, CMD_READ | (i == d->read_n - 1 ? CMD_STOP : 0));
    }
    d->busy = 1;
    d->issued_ms = d->last_issue_ms = now;
}

void touch_poll(void)
{
    int i;
    if (!touch_present) return;
    for (i = 0; i < NDEV; i++)
        poll_device(&devs[i]);
}
