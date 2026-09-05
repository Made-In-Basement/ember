/* input.c - mouse and keyboard for the shell.
 *
 * Both arrive as interrupts the kernel hands to us: the PS/2 mouse on
 * IRQ12 in three-byte packets, the keyboard on IRQ1 as scancodes.  Each
 * handler does the least it can and drops an event in a queue, so nothing
 * slow ever runs with interrupts disabled.
 */
#include <nanolibc.h>
#include "nano.h"
#include "input.h"

int mouse_x, mouse_y, mouse_buttons;
int mouse_present, mouse_via_bios;

#define STUB_OFF 0xF800                 /* a far-return stub in the bounce */

static volatile struct event queue[64];
static volatile int q_head, q_tail;
static int last_buttons;

static void push(int type, int a, int b)
{
    int next = (q_head + 1) % 64;
    if (next == q_tail) return;                 /* full: drop the oldest news */
    queue[q_head].type = type;
    queue[q_head].a = a;
    queue[q_head].b = b;
    q_head = next;
}

int next_event(struct event *e)
{
    if (q_tail == q_head) return 0;
    *e = *(struct event *)&queue[q_tail];
    q_tail = (q_tail + 1) % 64;
    return 1;
}

/* ---------------------------------------------------------------- mouse */
static uint8_t packet[3];
static int packet_n;

static void mouse_irq(void)
{
    uint8_t b = inb(0x60);
    int dx, dy, buttons;

    if (packet_n == 0 && !(b & 0x08))
        return;                                 /* out of step: wait for a start */
    packet[packet_n++] = b;
    if (packet_n < 3)
        return;
    packet_n = 0;

    buttons = packet[0] & 0x07;
    dx = packet[1];
    dy = packet[2];
    if (packet[0] & 0x10) dx |= ~0xFF;          /* the sign lives in byte 0 */
    if (packet[0] & 0x20) dy |= ~0xFF;
    if (packet[0] & 0xC0) return;               /* overflow: the packet is junk */

    mouse_x += dx;
    mouse_y -= dy;                              /* the mouse counts up, screens down */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > mouse_max_x) mouse_x = mouse_max_x;
    if (mouse_y > mouse_max_y) mouse_y = mouse_max_y;

    if (dx || dy)
        push(EV_MOUSE_MOVE, mouse_x, mouse_y);
    if ((buttons & 1) != (last_buttons & 1))
        push((buttons & 1) ? EV_MOUSE_DOWN : EV_MOUSE_UP, mouse_x, mouse_y);
    if ((buttons & 2) != (last_buttons & 2))
        push((buttons & 2) ? EV_RIGHT_DOWN : EV_RIGHT_UP, mouse_x, mouse_y);
    last_buttons = buttons;
    mouse_buttons = buttons;
}

int mouse_max_x, mouse_max_y;

/* the 8042, which is also how the keyboard is attached */
static void kbd_wait_in(void)
{
    int n = 100000;
    while (n-- && (inb(0x64) & 0x02)) ;
}

static void kbd_wait_out(void)
{
    int n = 100000;
    while (n-- && !(inb(0x64) & 0x01)) ;
}

static void aux_write(uint8_t v)
{
    kbd_wait_in();
    outb(0x64, 0xD4);                           /* the next byte is for the mouse */
    kbd_wait_in();
    outb(0x60, v);
    kbd_wait_out();
    inb(0x60);                                  /* its acknowledgement */
}

/* Send a command to the mouse and wait for its acknowledgement.  Returns 0
   if the device answered, which is how we tell there is one at all. */
static int aux_command(uint8_t v)
{
    int n;
    kbd_wait_in();
    outb(0x64, 0xD4);                           /* the next byte is for the mouse */
    kbd_wait_in();
    outb(0x60, v);
    for (n = 0; n < 400000; n++)
        if (inb(0x64) & 0x01)
            return inb(0x60) == 0xFA ? 0 : -1;
    return -1;
}

static int aux_read(uint8_t *out)
{
    int n;
    for (n = 0; n < 2000000; n++)
        if (inb(0x64) & 0x01) { *out = inb(0x60); return 0; }
    return -1;
}

static void aux_flush(void)
{
    int n;
    for (n = 0; n < 64 && (inb(0x64) & 0x01); n++)
        inb(0x60);
}

/* Ask the firmware to start its own mouse emulation.  We hand it a stub
   that does nothing but return, because we want the interrupt, not the
   BIOS's callback: it cannot call into protected mode anyway. */
static int bios_assist(void)
{
    struct rmcall r;
    /* a far return instruction, somewhere the BIOS can reach */
    nx_bounce[STUB_OFF] = 0xCB;

    memset(&r, 0, sizeof r);
    r.ax = 0xC205;                              /* initialise, 3-byte packets */
    r.bx = 0x0300;
    r.intno = 0x15;
    sys_bios(&r);
    if (r.flags & 1) return -1;

    memset(&r, 0, sizeof r);
    r.ax = 0xC207;                              /* the handler it should call */
    r.es = nx_bounce_seg;
    r.bx = STUB_OFF;
    r.intno = 0x15;
    sys_bios(&r);
    if (r.flags & 1) return -1;

    memset(&r, 0, sizeof r);
    r.ax = 0xC200;                              /* and switch it on */
    r.bx = 0x0100;
    r.intno = 0x15;
    sys_bios(&r);
    if (r.flags & 1) return -1;
    return 0;
}

int input_open(int width, int height)
{
    uint8_t status, b;
    mouse_max_x = width - 1;
    mouse_max_y = height - 1;
    mouse_x = width / 2;
    mouse_y = height / 2;
    packet_n = 0;

    aux_flush();
    kbd_wait_in();
    outb(0x64, 0xA8);                           /* switch the mouse port on */
    kbd_wait_in();
    outb(0x64, 0x20);                           /* read the controller's setup */
    kbd_wait_out();
    status = inb(0x60);
    status |= 0x02;                             /* let the mouse interrupt */
    status &= (uint8_t)~0x20;                   /* and stop ignoring its clock */
    kbd_wait_in();
    outb(0x64, 0x60);
    kbd_wait_in();
    outb(0x60, status);
    aux_flush();

    /* Is there really a mouse on that port?  A reset is answered by an
       acknowledgement, then AAh when it has tested itself. */
    mouse_via_bios = 0;
    if (aux_command(0xFF) == 0 && aux_read(&b) == 0 && b == 0xAA) {
        aux_read(&b);                           /* its identity, if it offers one */
        aux_command(0xF6);                      /* sensible defaults */
        aux_command(0xF4);                      /* start reporting */
        mouse_present = 1;
    } else if (bios_assist() == 0) {
        mouse_via_bios = 1;                     /* a USB mouse, through the BIOS */
        mouse_present = 1;
    } else {
        mouse_present = 0;
    }

    packet_n = 0;
    sys_set_mouse_handler(mouse_irq);
    return mouse_present ? 0 : -1;
}

void input_close(void)
{
    sys_set_mouse_handler(0);
    sys_set_irq_handlers(0, 0);
}

/* ---------------------------------------------------------------- keyboard */
static int shift_down, e0;

static const char plain[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char shifted[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ',
};

static void kbd_irq(void)
{
    uint8_t sc = inb(0x60);
    int ch = 0;
    if (sc == 0xE0) { e0 = 1; return; }
    if (sc & 0x80) {
        sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36) shift_down = 0;
        e0 = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; e0 = 0; return; }
    if (!e0 && sc < 128)
        ch = (shift_down ? shifted : plain)[sc];
    push(EV_KEY, sc | (e0 ? 0x100 : 0), ch);
    e0 = 0;
}

void input_start_keyboard(void)
{
    sys_set_irq_handlers(0, kbd_irq);
}
