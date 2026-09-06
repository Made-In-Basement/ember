/* input.c - mouse and keyboard for the shell.
 *
 * Both arrive as interrupts the kernel hands to us: the PS/2 mouse on
 * IRQ12 in three-byte packets, the keyboard on IRQ1 as scancodes.  Each
 * handler does the least it can and drops an event in a queue, so nothing
 * slow ever runs with interrupts disabled.
 *
 * On a laptop the "PS/2 mouse" is usually a USB one that the firmware is
 * impersonating from system management mode, and that impersonation is
 * imperfect: it may not flag its bytes as the mouse's, may answer commands
 * late or not at all, and traps every port access, which costs time.  So
 * this driver asks the device as little as possible, reads one byte per
 * interrupt, and keeps a record of what it saw for the log.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"

int mouse_x, mouse_y, mouse_buttons;
int mouse_present, mouse_via_bios;
static int listen_only;                 /* a touchpad points; PS/2 bytes are counted, not used */

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

/* ---------------------------------------------------------------- a record */
/* The first bytes to arrive, with where they came from, so that a mouse
   that misbehaves on a machine we cannot see can still be understood. */
#define TRACE_N 96
static struct { uint8_t status, byte, irq; unsigned ms; } trace[TRACE_N];
static int trace_n;
static unsigned n_irq12, n_irq1_mouse, n_packets, n_dropped, n_resync;
static char probe_note[120];

static void note(const char *s)
{
    size_t have = strlen(probe_note);
    if (have + strlen(s) + 1 < sizeof probe_note) {
        if (have) probe_note[have++] = ' ';
        strcpy(probe_note + have, s);
    }
}

/* ---------------------------------------------------------------- mouse */
static uint8_t packet[3];
static int packet_n;
static unsigned packet_ms;                  /* when the packet started */
static void mouse_irq(void);                /* the mouse's own interrupt */

static void mouse_byte(uint8_t b)
{
    int dx, dy, buttons;
    unsigned now = now_ms();

    /* A packet arrives all at once; a long pause means the last one was
       cut short, and this byte starts a new one.  Without this, one lost
       byte would leave every later packet read one byte out of step. */
    if (packet_n && now - packet_ms > 50) {
        packet_n = 0;
        n_resync++;
    }
    if (packet_n == 0) {
        /* The first byte always has bit 3 set and, short of the mouse
           being flung, bits 6 and 7 clear.  That also rejects FAh and AAh,
           a late acknowledgement or self-test report, which would
           otherwise be read as a click. */
        if ((b & 0xC8) != 0x08) {
            n_dropped++;
            return;
        }
        packet_ms = now;
    }
    packet[packet_n++] = b;
    if (packet_n < 3)
        return;
    packet_n = 0;
    n_packets++;

    buttons = packet[0] & 0x07;
    dx = packet[1];
    dy = packet[2];
    if (packet[0] & 0x10) dx |= ~0xFF;          /* the sign lives in byte 0 */
    if (packet[0] & 0x20) dy |= ~0xFF;
    input_inject_mouse(dx, -dy, buttons);       /* the mouse counts up, screens down */
}

/* Movement and buttons from any pointing device: dy is positive downwards.
   Called from an interrupt or from the main loop; either way it only
   moves the pointer and queues what changed. */
void input_inject_mouse(int dx, int dy, int buttons)
{
    /* A gentle acceleration: a slow movement stays precise, a quick one
       crosses the screen without a second push. */
    if (dx > 6 || dx < -6) dx *= 2;
    if (dy > 6 || dy < -6) dy *= 2;

    mouse_x += dx;
    mouse_y += dy;
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

/* Wait for the controller to have something to say, but not forever: on
   a machine with no PS/2 mouse nothing ever answers, and every millisecond
   spent here is a millisecond of blank screen at start-up. */
static int aux_wait(unsigned ms)
{
    unsigned until = now_ms() + ms;
    while (now_ms() < until)
        if (inb(0x64) & 0x01) return 0;
    return -1;
}

/* Send a command to the mouse and wait for its acknowledgement.  Returns 0
   if the device answered, which is how we tell there is one at all.  The
   wait is generous because an impersonated mouse answers slowly. */
static int aux_command(uint8_t v, unsigned ms)
{
    kbd_wait_in();
    outb(0x64, 0xD4);                           /* the next byte is for the mouse */
    kbd_wait_in();
    outb(0x60, v);
    if (aux_wait(ms) != 0) return -1;
    return inb(0x60) == 0xFA ? 0 : -1;
}

static int aux_read(uint8_t *out, unsigned ms)
{
    if (aux_wait(ms) != 0) return -1;
    *out = inb(0x60);
    return 0;
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
    if (r.flags & 1) { note("bios-init-refused"); return -1; }

    memset(&r, 0, sizeof r);
    r.ax = 0xC207;                              /* the handler it should call */
    r.es = nx_bounce_seg;
    r.bx = STUB_OFF;
    r.intno = 0x15;
    sys_bios(&r);
    if (r.flags & 1) { note("bios-handler-refused"); return -1; }

    memset(&r, 0, sizeof r);
    r.ax = 0xC200;                              /* and switch it on */
    r.bx = 0x0100;
    r.intno = 0x15;
    sys_bios(&r);
    if (r.flags & 1) { note("bios-enable-refused"); return -1; }
    note("bios-enabled");
    return 0;
}

/* passive: do not ask the PS/2 port anything, just listen.  Used when a
   touchpad is doing the pointing and the firmware's impersonated mouse is
   welcome to keep sending packets but not worth provoking: on one laptop
   commands to that port upset the firmware enough that the stick could
   no longer be written. */
int input_open(int width, int height, int passive)
{
    uint8_t status, b, mask;
    int tries;
    unsigned t0 = now_ms();
    mouse_max_x = width - 1;
    mouse_max_y = height - 1;
    mouse_x = width / 2;
    mouse_y = height / 2;
    packet_n = 0;
    probe_note[0] = 0;

    if (passive) {
        sys_log("mouse: listening only");
        listen_only = 1;
        mouse_present = 1;
        sys_set_mouse_handler(mouse_irq);
        return 0;
    }

    /* The key that started us is still coming up; let its release pass
       through the keyboard's interrupt rather than land in our probe. */
    while (now_ms() - t0 < 150) ;

    /* The mouse's replies raise IRQ12, and until our handler is in place
       the kernel's stub just discards whatever raised it.  So the
       interrupt is held back while we ask, and let go once we listen. */
    mask = inb(0xA1);
    outb(0xA1, mask | 0x10);

    kbd_wait_in();
    outb(0x64, 0xA8);                           /* switch the mouse port on */

    /* Read the controller's command byte.  A key going up meanwhile leaves
       its scancode in the same buffer, and written back as the setting
       that switches the keyboard off; so nothing else may read the port
       while we ask, and a byte with bit 7 set is a scancode, not a
       setting.  Without a believable answer the setting is left alone. */
    status = 0;
    for (tries = 0; tries < 4 && !status; tries++) {
        __asm__ volatile("cli");
        aux_flush();
        kbd_wait_in();
        outb(0x64, 0x20);
        kbd_wait_out();
        if (inb(0x64) & 0x01) {
            b = inb(0x60);
            if (!(b & 0x80)) status = b;
        }
        __asm__ volatile("sti");
    }
    if (status) {
        status |= 0x02;                         /* let the mouse interrupt */
        status &= (uint8_t)~0x20;               /* and stop ignoring its clock */
        kbd_wait_in();
        outb(0x64, 0x60);
        kbd_wait_in();
        outb(0x60, status);
    } else {
        note("no-command-byte");
    }
    aux_flush();

    /* Is there really a mouse on that port?  A reset is answered by an
       acknowledgement, then AAh when it has tested itself.  Nothing else
       is asked of it: the defaults are fine, and every extra command is
       another chance for a late answer to be mistaken for movement. */
    mouse_via_bios = 0;
    if (aux_command(0xFF, 250) != 0) {
        note("no-ack-to-reset");
    } else if (aux_read(&b, 1000) != 0 || b != 0xAA) {
        note("no-self-test");
    } else {
        note("reset-ok");
        if (aux_read(&b, 300) == 0) note("has-id");
        if (aux_command(0xF6, 250) != 0) note("no-ack-defaults");
        if (aux_command(0xF4, 250) != 0) note("no-ack-enable");
        mouse_present = 1;
    }
    if (!mouse_present) {
        if (bios_assist() == 0) {
            mouse_via_bios = 1;                 /* a USB mouse, through the BIOS */
            mouse_present = 1;
        }
    }
    sys_logf("mouse: %s in %u ms, controller byte %02X",
             probe_note, now_ms() - t0, status);

    aux_flush();                                /* anything said meanwhile */
    packet_n = 0;
    sys_set_mouse_handler(mouse_irq);
    outb(0xA1, mask & (uint8_t)~0x10);
    return mouse_present ? 0 : -1;
}

/* What was seen, for the log: the counts, then the first bytes with their
   status and which interrupt brought them. */
void input_close(void)
{
    char line[200];
    int i, n = 0;
    sys_set_mouse_handler(0);
    sys_set_irq_handlers(0, 0);
    sys_logf("mouse: %u on irq12, %u on irq1 flagged aux, %u packets, "
             "%u dropped out of step, %u resyncs after a pause",
             n_irq12, n_irq1_mouse, n_packets, n_dropped, n_resync);
    for (i = 0; i < trace_n; i++) {
        n += snprintf(line + n, sizeof line - n, "%s%u/%02X:%02X",
                      (i % 4) ? " " : "", trace[i].irq, trace[i].status,
                      trace[i].byte);
        if (i % 4 == 3 || i == trace_n - 1) {
            sys_logf("mouse: at +%ums %s", trace[i - (i % 4)].ms, line);
            n = 0;
        }
    }
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

static void key_byte(uint8_t sc)
{
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

static void record(uint8_t status, uint8_t b, int irq)
{
    if (trace_n < TRACE_N) {
        trace[trace_n].status = status;
        trace[trace_n].byte = b;
        trace[trace_n].irq = (uint8_t)irq;
        trace[trace_n].ms = now_ms();
        trace_n++;
    }
}

/* One byte per interrupt, like the driver that worked before this one.
   The keyboard and the mouse share the data port, and bit 5 of the status
   normally says which of them a byte came from; but on the mouse's own
   interrupt nothing else is expected, so the byte is the mouse's whether
   the firmware flags it or not. */
static void mouse_irq(void)
{
    uint8_t status = inb(0x64);
    uint8_t b;
    if (!(status & 0x01))
        return;                                 /* nothing there after all */
    b = inb(0x60);
    n_irq12++;
    record(status, b, 12);
    if (!listen_only)
        mouse_byte(b);
}

static void keyboard_irq(void)
{
    uint8_t status = inb(0x64);
    uint8_t b;
    if (!(status & 0x01))
        return;
    b = inb(0x60);
    if (status & 0x20) {
        n_irq1_mouse++;
        record(status, b, 1);
        mouse_byte(b);
    } else {
        key_byte(b);
    }
}

void input_start_keyboard(void)
{
    sys_set_irq_handlers(0, keyboard_irq);
}
