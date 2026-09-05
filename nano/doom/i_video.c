/* Doom for Ember: VGA mode 13h video and the keyboard */
#include <nanolibc.h>
#include "nano.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "d_event.h"
#include "v_video.h"
#include "i_system.h"
#include "i_video.h"

static byte *const vga = (byte *)0xA0000;

/* ---- keyboard: scancodes arrive from the IRQ1 handler ---- */
static volatile unsigned char kq[128];
static volatile unsigned kq_head, kq_tail;
unsigned kb_irq_count, kb_poll_count;
extern volatile uint32_t snd_dbg[16];   /* 13 = IRQ bytes, 14 = polled, 15 = last */

static void kq_push(unsigned char sc)
{
    if (kq_head - kq_tail < sizeof kq) kq[kq_head++ & 127] = sc;
}

void I_KeyboardIrq(void)
{
    /* only take a byte that is really there (polling may have taken it) */
    if (inb(0x64) & 0x01) {
        unsigned char sc = inb(0x60);
        kb_irq_count++;
        snd_dbg[13] = kb_irq_count;
        snd_dbg[15] = sc;
        kq_push(sc);
    }
}

/* Poll the 8042 too, in case IRQ1 does not reach protected mode on this
   machine.  Mouse bytes (status bit 5) are discarded. */
static void kb_poll(void)
{
    int n = 0;
    cli();
    while ((inb(0x64) & 0x01) && n++ < 16) {
        unsigned char st = inb(0x64);
        unsigned char sc = inb(0x60);
        if (!(st & 0x20)) {
            kb_poll_count++;
            snd_dbg[14] = kb_poll_count;
            snd_dbg[15] = sc;
            kq_push(sc);
        }
    }
    sti();
}

static const unsigned char sc2key[128] = {
/* 00 */ 0, KEY_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', KEY_MINUS, KEY_EQUALS, KEY_BACKSPACE, KEY_TAB,
/* 10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', KEY_ENTER, KEY_RCTRL, 'a', 's',
/* 20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', KEY_RSHIFT, '\\', 'z', 'x', 'c', 'v',
/* 30 */ 'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT, '*', KEY_RALT, ' ', 0, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
/* 40 */ KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_PAUSE, 0, 0, KEY_UPARROW, 0, KEY_MINUS, KEY_LEFTARROW, '5', KEY_RIGHTARROW, KEY_EQUALS, 0,
/* 50 */ KEY_DOWNARROW, 0, 0, 0, 0, 0, 0, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0,
};

void I_StartTic(void)
{
    static int ext;
    event_t ev;
    kb_poll();
    while (kq_tail != kq_head) {
        unsigned char sc = kq[kq_tail++ & 127];
        int key;
        if (sc == 0xE0) { ext = 1; continue; }
        if (sc == 0xE1) continue;
        key = sc2key[sc & 0x7F];
        if (ext && (sc & 0x7F) == 0x35) key = '/';
        ext = 0;
        if (!key) continue;
        ev.type = (sc & 0x80) ? ev_keyup : ev_keydown;
        ev.data1 = key;
        ev.data2 = ev.data3 = 0;
        D_PostEvent(&ev);
    }
}

void I_StartFrame(void)
{
}

/* ---- video ---- */
void I_InitGraphics(void)
{
    sys_set_video_mode(0x13);
    screens[0] = malloc(SCREENWIDTH * SCREENHEIGHT);
}

void I_ShutdownGraphics(void)
{
    sys_set_video_mode(3);
}

void I_SetPalette(byte *palette)
{
    int i;
    byte *gamma = gammatable[usegamma];
    outb(0x3C8, 0);
    for (i = 0; i < 256; i++) {
        outb(0x3C9, gamma[*palette++] >> 2);
        outb(0x3C9, gamma[*palette++] >> 2);
        outb(0x3C9, gamma[*palette++] >> 2);
    }
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
    /* wait for the start of vertical retrace to limit tearing */
    while (inb(0x3DA) & 8) ;
    while (!(inb(0x3DA) & 8)) ;
    memcpy(vga, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}
