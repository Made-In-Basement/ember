/* power.c - the battery, read from the embedded controller.
 *
 * The firmware on the laptops we know offers no power services, but the
 * embedded controller keeps the battery's figures in its own small
 * memory, read a byte at a time through two ports: a command to port
 * 66h, an address to port 62h, the byte back from 62h, with a busy flag
 * to wait on between.  Where each figure lives came from the firmware's
 * own table (docs/HARDWARE.md); it is the layout Lenovo has used for a
 * long time.  On a machine without such a controller nothing answers,
 * the first attempt gives up after a few milliseconds, and nothing asks
 * again.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "power.h"

#define EC_DATA   0x62
#define EC_CMD    0x66
#define EC_IBF    0x02
#define EC_OBF    0x01

static int state = -1;                  /* -1 untried, 0 none, 1 answering */
static int pct = -1, mains = -1, charging = -1, cycles = -1;
static unsigned last_ms;

static int ec_wait(int mask, int want)
{
    unsigned t0 = now_us();
    while ((inb(EC_CMD) & mask) != want)
        if (now_us() - t0 > 20000) return -1;   /* 20 ms: it is not answering */
    return 0;
}

static int ec_read(int addr)
{
    if (ec_wait(EC_IBF, 0) != 0) return -1;
    outb(EC_CMD, 0x80);                         /* read a byte */
    if (ec_wait(EC_IBF, 0) != 0) return -1;
    outb(EC_DATA, (uint8_t)addr);
    if (ec_wait(EC_OBF, EC_OBF) != 0) return -1;
    return inb(EC_DATA);
}

static int ec_read16(int addr)
{
    int lo = ec_read(addr), hi = ec_read(addr + 1);
    if (lo < 0 || hi < 0) return -1;
    return lo | (hi << 8);
}

/* 67.0 mains present; 96: bit 0 battery present, bit 2 charging, bit 4
   discharging; 106 remaining, 108 full, 164 cycles */
static void sample(void)
{
    int rc = ec_read16(106), fc = ec_read16(108), st = ec_read(96), ac = ec_read(67);
    if (rc < 0 || fc <= 0 || rc == 0xFFFF || fc == 0xFFFF || rc > fc * 12 / 10) {
        if (state < 0) state = 0;               /* never answered: leave it */
        return;
    }
    state = 1;
    pct = rc * 100 / fc;
    if (pct > 100) pct = 100;
    mains = ac >= 0 ? (ac & 1) : -1;
    charging = st >= 0 ? ((st & 4) != 0) : -1;
    cycles = ec_read16(164);
}

/* from the main loop: a reading every ten seconds, the first at once */
void power_tick(void)
{
    unsigned now = now_ms();
    if (state == 0) return;
    if (state < 0 || now - last_ms > 10000) {
        sample();
        last_ms = now;
    }
}

int power_known(void)     { return state == 1; }
int power_percent(void)   { return state == 1 ? pct : -1; }
int power_on_mains(void)  { return state == 1 ? mains : -1; }
int power_charging(void)  { return state == 1 ? charging : -1; }
int power_cycles(void)    { return state == 1 ? cycles : -1; }
