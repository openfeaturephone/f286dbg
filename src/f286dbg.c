/* f286dbg.c - on-phone debug agent: a request/reply protocol over the USB serial pipe (UCOM
 * channel 0 of USB service 4) so the PC can read/write memory, call functions, upload and run
 * apps, press keys. Wire protocol: PROTOCOL.md. PC client: tools/f286.py.
 *
 * Deployed as D:\apps\launcher.bin (or E:\apps\1.bin); open Apps -> Facebook. RSOFT/RED exit.
 *
 * Phone-safety rules followed here (each has reset the phone before):
 *  - USB_START(4), then UCOM channels 0-3 opened at once (the 5 s detect timer calls all four
 *    callbacks), all four closed before the window goes away.
 *  - No unaligned access (build.py passes +strict-align); wire fields are read/written bytewise.
 *  - Text lines are <= 20 characters.
 *  - TX buffers: UCOM write (UCOM_OPS[4] -> kernel 0x80afaa38 -> 0x80afa3f0) only stores the
 *    caller's pointer in the pipe's transfer descriptor and submits it, i.e. the USB controller
 *    reads our buffer AFTER write() returns, and a write while the previous transfer is still
 *    busy is silently dropped (descriptor+0x20 != 0 -> nothing happens, write() still returns len).
 *    So: replies sit in a ring of NSLOT buffers, only one transfer is ever in flight, the next
 *    write starts only after the busy flag (descriptor+0x20) clears, queue depth is limited to
 *    NSLOT-1 so the buffer in flight is never refilled, and each buffer is D-cache cleaned
 *    after filling. If the busy flag cannot be located (pointer sanity checks fail) a 3 ms
 *    guard between writes is used instead; a transfer stuck busy for 1 s is treated as done. */
#include "f286_api.h"
#include "app_rt.h"

F286_APP_HEADER(f286_main);

#define WIN_ID 0x00f28610u
#define MSG_GET_FOCUS 0xf024u    /* likely: 0xf021..0xf026 = open/lose focus/get focus/paint family */

#define USB_START   ((uint32_t (*)(uint32_t))0x81329de3u)
#define USB_STOP    ((uint32_t (*)(uint32_t))0x81329e5fu)
#define USB_CURRENT ((uint32_t (*)(void))0x81329dddu)
#define UCOM_OPS    ((void *const *)0x814c8154u)
/* KPD service path, used by
 * CMD_KEYPAD/CMD_WAKE to inject a real key instead of only posting a window message:
 *   kernel 0x81179d7e(rec, 0, 0, 0): rec = {u16 keymap_index, u16 edge (0x81 down, 0x80 up)}.
 * It calls 0x8117a0f4(rec[0]) internally to turn the keymap INDEX back into an SCI_VK
 * and posts the key signal to Q_KPDSVR -> Q_P_APP, exactly as the real ISR does, so this
 * reaches MMIDEFAULT_ConvertKeyCode and the focused window like a hardware key press.
 * Found by decompiling only (no reference name for it). Phone-verified: press_key drives the
 * stock UI (the USB-mode popup, idle, Contacts). */
#define KPD_DISPATCH ((void (*)(const uint16_t *, uint32_t, uint32_t, uint32_t))0x81179d7fu)
#define KPD_KEYMAP_PTR (*(const uint16_t *const *)0x812189dcu)   /* 64-entry SCI_VK ROM table */
#define VK_WAKE 7u   /* RIGHT: WAKE presses this. At idle UP/DOWN/OK open the main menu, LEFT/RIGHT do nothing */
/* kernel cache maintenance, ARM mode (bit 0 clear), same as stub.c */
#define DCACHE_CLEAN_RANGE ((void (*)(uint32_t, uint32_t))0x814c3f88u)   /* (start, end) */
#define ICACHE_INVAL_RANGE ((void (*)(uint32_t, uint32_t))0x814c4120u)   /* (addr, len) */
/* TX transfer descriptor lookup (from the UCOM write path decompile): pipe = [0x817040d0 + 4*ch],
 * descriptor ptr = [0x818fc800 + pipe*0x20], busy flag = [descriptor + 0x20] */
#define TX_PIPE_TAB 0x817040d0u
#define TX_DESC_TAB 0x818fc800u

typedef uint32_t (*ucom_cb_t)(uint32_t event);
typedef uint32_t (*op_open_t)(uint32_t dev, uint32_t cfg, ucom_cb_t cb);
typedef uint32_t (*op_rw_t)(uint32_t dev, void *buf, uint32_t len);
typedef void (*op_ctl_t)(uint32_t dev, uint32_t on);
typedef uint64_t (*call4_t)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint64_t (*call12_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

#define PROTO_VER 2
#define TICK_MS   20
#define TICK_IDLE_MS 1000u  /* with no USB traffic for ACTIVE_MS: one wake a second instead of fifty (battery) */
#define ACTIVE_MS 3000u     /* fast ticks for this long after the last byte in or out */
#define MAXPAY    0x7f0u
#define RXSZ      0x1800u
#define TXS       0x800u
#define NSLOT     4u
#define SCRSZ     0x800u
#define MEMSZ     (RXSZ + NSLOT * TXS + SCRSZ + 32u)
#define BOOT_OK_MS 60000u   /* uptime (or first USB connect, whichever first) that clears D:\F286_BOOTCNT */

enum {
    CMD_PING = 1, CMD_READ, CMD_WRITE, CMD_CALL, CMD_ALLOC, CMD_FREE, CMD_CACHE, CMD_KEY, CMD_EXIT,
    CMD_GETAPI,
    /* protocol v2 (the commands above are unchanged, so a v1 client keeps working) */
    CMD_CALL8, CMD_WAKE, CMD_KEYPAD, CMD_USBMODE,
    CMD_LOG = 0x7f
};

typedef struct {
    const f286_api_t *api;
    void *base;
    uint32_t size;
    uint8_t *rx, *txb, *scr;
    void *mem;
    uint32_t rx_len;
    uint32_t rx_total, tx_total, ncmd, nerr, ndrop;
    uint32_t t_write, t_exit, t_paint;
    uint32_t last_rc;
    uint32_t ev[4];
    uint16_t txlen[NSLOT];
    uint8_t head, tail, count, wr_pending;
    uint8_t timer, open_mask, conn, exiting;
    uint8_t closed, dirty, log_conn, painted;
    uint8_t unfocused, pad2, pad3, pad4;
    uint8_t last_cmd, last_status, last_seq, pad;
    /* boot-counter clear (mocor-sdk stub.c boot_counter_bump) and deferred USBMODE */
    uint32_t t_start;
    uint8_t boot_ok_done, usbmode_pending, pad5, pad6;
    uint32_t t_usbmode;
    /* adaptive tick */
    uint32_t t_active, seen_rx, seen_tx;
} state_t;
APP_STATE(state_t);

static uint32_t now_ms(void) { return *(volatile uint32_t *)0x4023000cu; }

static void crumb(const f286_api_t *api, uint32_t step, uint32_t value) { api->crumb(step, value); }

/* ---- little-endian byte access (never a wide access to a possibly unaligned address) ---- */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* ---- memory access with the widest aligned access that fits (MMIO needs 32-bit reads) ---- */
static void mem_read(uint8_t *dst, uint32_t addr, uint32_t len)
{
    while (len) {
        if (!(addr & 3) && len >= 4) {
            wr32(dst, *(volatile uint32_t *)addr);
            dst += 4; addr += 4; len -= 4;
        } else if (!(addr & 1) && len >= 2) {
            wr16(dst, *(volatile uint16_t *)addr);
            dst += 2; addr += 2; len -= 2;
        } else {
            *dst++ = *(volatile uint8_t *)addr;
            addr++; len--;
        }
    }
}

static void mem_write(uint32_t addr, const uint8_t *src, uint32_t len)
{
    while (len) {
        if (!(addr & 3) && len >= 4) {
            *(volatile uint32_t *)addr = rd32(src);
            src += 4; addr += 4; len -= 4;
        } else if (!(addr & 1) && len >= 2) {
            *(volatile uint16_t *)addr = (uint16_t)rd16(src);
            src += 2; addr += 2; len -= 2;
        } else {
            *(volatile uint8_t *)addr = *src++;
            addr++; len--;
        }
    }
}

static void dclean(const void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~31u, e = ((uint32_t)p + n + 31u) & ~31u;
    DCACHE_CLEAN_RANGE(a, e);
}

/* ---- TX ring ---- */
/* 1 busy, 0 idle, -1 unknown (descriptor pointer failed the sanity checks) */
static int tx_busy_flag(void)
{
    uint32_t pipe = *(volatile uint32_t *)TX_PIPE_TAB;
    if (pipe >= 10)
        return -1;
    uint32_t dp = *(volatile uint32_t *)(TX_DESC_TAB + pipe * 0x20u);
    if ((dp & 3) || dp < 0x81700000u || dp >= 0x83000000u)
        return -1;
    return *(volatile uint32_t *)(dp + 0x20u) != 0;
}

static int tx_idle(state_t *s)
{
    if (!s->wr_pending)
        return 1;
    uint32_t dt = now_ms() - s->t_write;
    int b = tx_busy_flag();
    if ((b == 0) || (b < 0 && dt >= 3) || dt >= 1000) {
        s->wr_pending = 0;
        return 1;
    }
    return 0;
}

static void tx_flush(state_t *s)
{
    while (s->count && (s->conn & 1) && tx_idle(s)) {
        uint8_t *r = s->txb + (uint32_t)s->head * TXS;
        uint32_t len = s->txlen[s->head];
        uint32_t n = ((op_rw_t)UCOM_OPS[4])(0, r, len);
        s->last_rc = n;
        if (n != len)
            return;                         /* refused (channel not connected): retry later */
        s->tx_total += n;
        s->head = (uint8_t)((s->head + 1) % NSLOT);
        s->count--;
        s->wr_pending = 1;
        s->t_write = now_ms();
        s->dirty = 1;
    }
}

/* next free slot for a frame, or 0 when the queue is full (depth NSLOT-1: the slot in flight is never reused) */
static uint8_t *slot_get(state_t *s)
{
    if (s->count >= NSLOT - 1) {
        s->ndrop++;
        s->dirty = 1;
        return 0;
    }
    return s->txb + (uint32_t)s->tail * TXS;
}

static void slot_commit(state_t *s, uint8_t *r, uint32_t total)
{
    if ((total & 63) == 0)                  /* never end on a full USB packet (no ZLP): pad one byte */
        r[total++] = 0;
    dclean(r, total);
    s->txlen[s->tail] = (uint16_t)total;
    s->tail = (uint8_t)((s->tail + 1) % NSLOT);
    s->count++;
}

static void queue_log(state_t *s, const char *msg)
{
    uint8_t *r = slot_get(s);
    if (!r)
        return;
    uint32_t n = 0;
    while (msg[n] && n < 200) {
        r[6 + n] = (uint8_t)msg[n];
        n++;
    }
    r[0] = 'F'; r[1] = 'D'; r[2] = CMD_LOG; r[3] = 0;
    wr16(r + 4, n);
    slot_commit(s, r, 6 + n);
}

/* ---- real keypad injection (KPD service path: Q_KPDSVR -> Q_P_APP), used by
 * CMD_KEYPAD/CMD_WAKE. vk is an SCI_VK code (f286_api.h key table); the keymap
 * lookup reads the 64-entry ROM table through the same literal pointer the kernel
 * itself reads. Returns 0 if vk is not in the keymap (e.g. RED/power, which is wired
 * to an EIC line, not the KPD matrix). */
static int kpd_inject(uint32_t vk, int down)
{
    const uint16_t *tab = KPD_KEYMAP_PTR;
    uint32_t idx = 0xffffffffu;
    for (uint32_t i = 0; i < 64; i++) {
        if (tab[i] == vk) {
            idx = i;
            break;
        }
    }
    if (idx == 0xffffffffu)
        return 0;
    uint16_t rec[2];
    rec[0] = (uint16_t)idx;
    rec[1] = down ? 0x81u : 0x80u;
    KPD_DISPATCH(rec, 0, 0, 0);
    return 1;
}

/* ---- commands ---- */
static void do_exit(state_t *s);

static void handle(state_t *s, uint8_t cmd, uint8_t seq, const uint8_t *p, uint32_t n)
{
    const f286_api_t *api = s->api;
    uint8_t *r = slot_get(s);
    if (!r)
        return;                             /* caller checked; defensive */
    uint8_t *o = r + 7;
    uint8_t st = 0;
    uint32_t ol = 0;
    switch (cmd) {
    case CMD_PING:
        wr16(o, PROTO_VER); wr32(o + 2, api->version); wr32(o + 6, now_ms());
        ol = 10;
        break;
    case CMD_READ: {
        uint32_t len = n >= 6 ? rd16(p + 4) : 0xffffu;
        if (n < 6 || len > MAXPAY) { st = 2; break; }
        mem_read(o, rd32(p), len);
        ol = len;
        break;
    }
    case CMD_WRITE:
        if (n < 4) { st = 2; break; }
        mem_write(rd32(p), p + 4, n - 4);
        wr16(o, n - 4);
        ol = 2;
        break;
    case CMD_CALL: {
        if (n < 4) { st = 2; break; }
        uint32_t a[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < 4 && n >= 8 + 4 * i; i++)
            a[i] = rd32(p + 4 + 4 * i);
        uint64_t rv = ((call4_t)rd32(p))(a[0], a[1], a[2], a[3]);
        wr32(o, (uint32_t)rv); wr32(o + 4, (uint32_t)(rv >> 32));
        ol = 8;
        break;
    }
    case CMD_ALLOC: {
        if (n < 4) { st = 2; break; }
        void *m = api->alloc(rd32(p));
        if (!m) { st = 1; break; }
        wr32(o, (uint32_t)m);
        ol = 4;
        break;
    }
    case CMD_FREE:
        if (n < 4) { st = 2; break; }
        api->free((void *)rd32(p));
        break;
    case CMD_CACHE: {
        if (n < 8) { st = 2; break; }
        uint32_t a = rd32(p), len = rd32(p + 4);
        DCACHE_CLEAN_RANGE(a, a + len);
        ICACHE_INVAL_RANGE(a, len);
        break;
    }
    case CMD_KEY: {
        if (n < 8) { st = 2; break; }
        uint32_t win = rd32(p);
        wr32(o, api->MMK_PostMsg(win ? win : WIN_ID, rd32(p + 4), 0, 0));
        ol = 4;
        break;
    }
    case CMD_EXIT:
        s->exiting = 1;
        s->t_exit = now_ms();
        break;
    case CMD_GETAPI:
        wr32(o, (uint32_t)api); wr32(o + 4, (uint32_t)s->base); wr32(o + 8, s->size);
        ol = 12;
        break;
    case CMD_CALL8: {
        /* like CALL, but up to 8 more args passed on the stack (AAPCS: a declared 12-arg
         * function type always passes exactly 12 words regardless of what the real callee
         * reads, so this is safe for any function needing <= 12 args). */
        if (n < 4) { st = 2; break; }
        uint32_t a[12] = { 0 };
        for (uint32_t i = 0; i < 12 && n >= 8 + 4 * i; i++)
            a[i] = rd32(p + 4 + 4 * i);
        uint64_t rv = ((call12_t)rd32(p))(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                          a[8], a[9], a[10], a[11]);
        wr32(o, (uint32_t)rv); wr32(o + 4, (uint32_t)(rv >> 32));
        ol = 8;
        break;
    }
    case CMD_WAKE:
        kpd_inject(VK_WAKE, 1);
        kpd_inject(VK_WAKE, 0);
        break;
    case CMD_KEYPAD: {
        if (n < 2) { st = 2; break; }
        if (!kpd_inject(p[0], p[1] != 0))
            st = 1;
        break;
    }
    case CMD_USBMODE: {
        if (n < 1 || p[0] > 1) { st = 2; break; }
        s->usbmode_pending = (uint8_t)(p[0] + 1);
        s->t_usbmode = now_ms();
        break;
    }
    default:
        st = 0xff;
        s->nerr++;
        break;
    }
    r[0] = 'F'; r[1] = 'D'; r[2] = cmd | 0x80; r[3] = seq;
    wr16(r + 4, 1 + ol);
    r[6] = st;
    slot_commit(s, r, 7 + ol);
    s->ncmd++;
    s->last_cmd = cmd; s->last_seq = seq; s->last_status = st;
    s->dirty = 1;
}

static void rx_drop(state_t *s, uint32_t k)
{
    if (k >= s->rx_len) {
        s->rx_len = 0;
        return;
    }
    for (uint32_t i = 0; i + k < s->rx_len; i++)
        s->rx[i] = s->rx[i + k];
    s->rx_len -= k;
}

/* reassembly: 'F','D',cmd,seq,len16,payload; resync on garbage; frames may be split or batched */
static void process(state_t *s)
{
    for (;;) {
        uint32_t n = s->rx_len;
        const uint8_t *b = s->rx;
        if (n >= 1 && b[0] != 'F') { rx_drop(s, 1); continue; }
        if (n >= 2 && b[1] != 'D') { rx_drop(s, 1); continue; }
        if (n < 6)
            return;
        uint32_t len = rd16(b + 4);
        if (len > MAXPAY) { s->nerr++; rx_drop(s, 2); continue; }
        if (n < 6 + len)
            return;
        if (s->count >= NSLOT - 1)
            return;                         /* no room for a reply yet: leave it queued */
        handle(s, b[2], b[3], b + 6, len);
        rx_drop(s, 6 + len);
        if (s->exiting)
            return;
    }
}

static void pump(uint32_t ch) { ((op_ctl_t)UCOM_OPS[2])(ch, 1); }

/* one service pass: pump RX, run commands, send replies; repeat while data keeps arriving */
static void service(state_t *s)
{
    for (uint32_t ch = 1; ch < 4; ch++)
        pump(ch);
    for (uint32_t iter = 0; iter < 16 && !s->exiting; iter++) {
        uint32_t before = s->rx_total;
        pump(0);
        process(s);
        tx_flush(s);
        if (s->rx_total != before)
            continue;
        if (!s->rx_len)
            break;
        /* partial frame pending: poll up to 8 ms for the rest of it */
        uint32_t t0 = now_ms();
        int got = 0;
        for (uint32_t polls = 0; polls < 4000 && now_ms() - t0 < 8; polls++) {
            pump(0);
            if (s->rx_total != before) { got = 1; break; }
        }
        if (!got)
            break;
    }
}

/* ---- UCOM callbacks (one per channel: the kernel passes only the event) ---- */
static uint32_t on_event(uint32_t ch, uint32_t event)
{
    state_t *s = STATE();
    if (event < 4)
        s->ev[event]++;
    if (event == 1) {
        s->conn |= (uint8_t)(1u << ch);
        s->dirty = 1;
        if (ch == 0)
            s->log_conn = 1;
    }
    if (event != 0 || !(s->open_mask & (1u << ch)) || !s->rx)
        return 0;
    if (ch != 0)
        return ((op_rw_t)UCOM_OPS[3])(ch, s->scr, SCRSZ);      /* channels 1-3: drain and discard */
    uint32_t room = RXSZ - s->rx_len;
    if (!room)
        return 0;
    uint32_t n = ((op_rw_t)UCOM_OPS[3])(0, s->rx + s->rx_len, room);
    if (n <= room) {
        s->rx_len += n;
        s->rx_total += n;
        s->dirty = 1;
    }
    return n;
}
static uint32_t cb0(uint32_t e) { return on_event(0, e); }
static uint32_t cb1(uint32_t e) { return on_event(1, e); }
static uint32_t cb2(uint32_t e) { return on_event(2, e); }
static uint32_t cb3(uint32_t e) { return on_event(3, e); }

static ucom_cb_t chan_cb(uint32_t ch)
{
    switch (ch) {
    case 0: return cb0;
    case 1: return cb1;
    case 2: return cb2;
    default: return cb3;
    }
}

static void close_all(state_t *s)
{
    for (uint32_t ch = 0; ch < 4; ch++)
        if (s->open_mask & (1u << ch))
            ((op_ctl_t)UCOM_OPS[1])(ch, 0);     /* fires cb(3), clears the channel */
    s->open_mask = 0;
}

static void do_exit(state_t *s)
{
    const f286_api_t *api = s->api;
    if (s->closed)
        return;
    s->closed = 1;
    close_all(s);
    if (s->timer)
        api->MMK_StopTimer(s->timer);
    s->timer = 0;
    crumb(api, 0x8f, s->ncmd);
    if (s->mem)
        api->free(s->mem);                      /* channels are closed: nothing reads these any more */
    s->mem = 0;
    s->rx = s->txb = s->scr = 0;
    api->MMK_CloseWin(WIN_ID);
}

/* ---- screen ---- */
typedef struct { uint16_t b[48]; int n; } line_t;
__attribute__((noinline)) static void ls(line_t *l, const char *s) { while (*s && l->n < 47) l->b[l->n++] = (uint8_t)*s++; l->b[l->n] = 0; }
__attribute__((noinline)) static void lx(line_t *l, uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0 && l->n < 47; i--) l->b[l->n++] = hex[(v >> (4 * i)) & 0xf];
    l->b[l->n] = 0;
}

static void text(const f286_api_t *api, int y, const uint16_t *s, GUI_COLOR_T color)
{
    static const GUI_LCD_DEV_INFO dev = { 0, 0 };
    int bottom = y + 20 > 319 ? 319 : y + 20;
    GUI_RECT_T r = { 4, (int16_t)y, 235, (int16_t)bottom };
    uint16_t n = wlen(s);
    MMI_STRING_T str = { (uint16_t *)s, n > 20 ? 20 : n };     /* never more than 20 characters */
    GUISTR_STYLE_T style;
    zero(&style, sizeof style);
    style.font = 16;
    style.font_color = color;
    style.align = 6;
    api->GUISTR_DrawTextToLCDInRect(&dev, &r, &r, &str, &style, 0, -1);
}

static void paint(state_t *s)
{
    const f286_api_t *api = s->api;
    static const GUI_LCD_DEV_INFO dev = { 0, 0 };
    GUI_RECT_T all = { 0, 0, 239, 319 };
    api->GUI_FillRect(&dev, all, 0x0000);
    line_t l;
    text(api, 0, u"F286 debug agent", 0xffe0);
    l.n = 0; ls(&l, s->conn & 1 ? "USB ch0 connected" : "USB ch0 waiting");
    text(api, 30, l.b, s->conn & 1 ? 0x07e0 : 0xffff);
    l.n = 0; ls(&l, "rx "); lx(&l, s->rx_total, 6); ls(&l, " tx "); lx(&l, s->tx_total, 6);
    text(api, 54, l.b, 0xffff);
    l.n = 0; ls(&l, "cmds "); lx(&l, s->ncmd, 4); ls(&l, " err "); lx(&l, s->nerr, 4);
    text(api, 78, l.b, 0xffff);
    l.n = 0; ls(&l, "last "); lx(&l, s->last_cmd, 2); ls(&l, " st "); lx(&l, s->last_status, 2);
    ls(&l, " #"); lx(&l, s->last_seq, 2);
    text(api, 102, l.b, 0xffff);
    l.n = 0; ls(&l, "drop "); lx(&l, s->ndrop, 4); ls(&l, " q "); lx(&l, s->count, 1);
    text(api, 126, l.b, 0xc618);
    text(api, 296, u"RSOFT/RED exit", 0xc618);
}

static void usb_up(state_t *s)
{
    uint32_t cur = USB_CURRENT();
    if (cur)
        USB_STOP(cur);
    s->last_rc = USB_START(4);
    for (uint32_t ch = 0; ch < 4; ch++) {       /* all four at once, before the 5 s detect timer */
        ((op_open_t)UCOM_OPS[0])(ch, 0, chan_cb(ch));
        s->open_mask |= (uint8_t)(1u << ch);
    }
}

/* CMD_USBMODE's ack (and any queued log) must be flushed to the host before the
 * channels go away, so the switch itself happens here, a tick after the handler set
 * usbmode_pending, once the TX ring has drained. mode 0 = serial: USB_STOP + usb_up(),
 * the same proven sequence as boot, so the agent reconnects ~5 s later like a fresh
 * boot. mode 1 = udisk: USB_START(1); the type number for plain U Disk was not pinned
 * down (only the UCOM mapping of types 2 and 4 is confirmed) - UNVERIFIED,
 * and once the channels close there is no way back to serial without a reboot. */
static void usbmode_apply(state_t *s)
{
    uint32_t target = (uint32_t)(s->usbmode_pending - 1);
    s->usbmode_pending = 0;
    close_all(s);
    if (target == 0) {
        usb_up(s);
    } else {
        uint32_t cur = USB_CURRENT();
        if (cur)
            USB_STOP(cur);
        USB_START(1u);
    }
}

/* ---- timer tick ---- */
static void tick(state_t *s)
{
    const f286_api_t *api = s->api;
    if (s->closed)
        return;
    if (s->log_conn) {
        s->log_conn = 0;
        crumb(api, 0x82, 0);
        queue_log(s, "f286dbg connected");
    }
    if (!s->boot_ok_done && (now_ms() - s->t_start >= BOOT_OK_MS || (s->conn & 1))) {
        /* survived long enough (or connected): clear the crash-loop counter stub.c bumps
         * at boot (mocor-sdk stub/stub.c, boot_counter_bump). */
        s->boot_ok_done = 1;
        static const uint16_t cnt_path[] = u"D:\\F286_BOOTCNT";
        uint32_t h = api->SFS_CreateFile(cnt_path, SFS_MODE_CREATE_ALWAYS_WRITE, 0, 0);
        if (h) {
            uint8_t z = 0;
            uint32_t w = 0;
            api->SFS_WriteFile(h, &z, 1, &w, 0);
            api->SFS_CloseFile(h);
        }
        queue_log(s, "boot ok, counter cleared");
    }
    tx_flush(s);
    service(s);
    if (s->exiting && ((!s->count && tx_idle(s)) || now_ms() - s->t_exit > 500)) {
        do_exit(s);
        return;
    }
    if (s->usbmode_pending && !s->count && tx_idle(s) && now_ms() - s->t_usbmode >= 20) {
        usbmode_apply(s);
        return;
    }
    /* repaint only while we are the focus window: another window (e.g. an uploaded app) is on top otherwise */
    if (s->dirty && !s->unfocused && now_ms() - s->t_paint >= 250) {
        s->dirty = 0;
        s->t_paint = now_ms();
        api->MMK_PostMsg(WIN_ID, MSG_FULL_PAINT, 0, 0);
    }
}

/* the next tick: fast while anything moves or waits, slow when the link is quiet. The UCOM callback
 * buffers incoming bytes itself, so a quiet agent answers the first command within TICK_IDLE_MS
 * (well inside f286.py's 2 s timeout) and is fast again from then on. */
static uint32_t next_tick(state_t *s)
{
    uint32_t t = now_ms();
    if (s->rx_total != s->seen_rx || s->tx_total != s->seen_tx) {
        s->seen_rx = s->rx_total;
        s->seen_tx = s->tx_total;
        s->t_active = t;
    }
    if (s->count || s->rx_len || !tx_idle(s) || s->exiting || s->usbmode_pending || s->log_conn ||
        (s->dirty && !s->unfocused) || t - s->t_active < ACTIVE_MS)
        return TICK_MS;
    return TICK_IDLE_MS;
}

static uint32_t handler(uint32_t win_id, uint32_t msg, void *param)
{
    state_t *s = STATE();
    const f286_api_t *api = s->api;
    switch (msg) {
    case MSG_OPEN_WINDOW:
        crumb(api, 0x81, win_id);
        usb_up(s);
        crumb(api, 0x84, s->open_mask);
        s->dirty = 1;
        api->MMK_PostMsg(win_id, MSG_FULL_PAINT, 0, 0);
        return 1;
    case MSG_CLOSE_WINDOW:
        if (!s->closed) {
            s->closed = 1;
            close_all(s);
            if (s->timer)
                api->MMK_StopTimer(s->timer);
            s->timer = 0;
        }
        return 1;
    case MSG_TIMER:
        if (param && s->timer && *(uint8_t *)param == s->timer) {
            s->timer = 0;
            tick(s);
            if (!s->closed)
                s->timer = api->MMK_CreateWinTimer(win_id, next_tick(s), 0, 0, 0);
            return 1;
        }
        return 0;
    case MSG_LOSE_FOCUS:
        s->unfocused = 1;
        return 1;
    case MSG_GET_FOCUS:
        s->unfocused = 0;
        s->dirty = 1;
        return 1;
    case MSG_FULL_PAINT:
        if (s->closed)
            return 1;
        if (s->unfocused && s->painted)         /* we were covered: a stock paint to the focus window means we are back */
            s->unfocused = 0;
        paint(s);
        if (!s->painted) {                      /* arm the timer once the window is fully up */
            s->painted = 1;
            s->timer = api->MMK_CreateWinTimer(win_id, TICK_MS, 0, 0, 0);
        }
        return 1;
    case KEY_PRESSED | KEY_RSOFT:
    case KEY_PRESSED | KEY_RED:
        do_exit(s);
        return 1;
    default:
        return 0;
    }
}

void f286_main(const f286_api_t *api, void *base, uint32_t size)
{
    if (api->version < 2)
        return;
    state_t *s = STATE();
    zero(s, sizeof *s);
    s->api = api;
    s->base = base;
    s->size = size;
    crumb(api, 0x80, (uint32_t)base);
    if (api->MMK_IsOpenWin(WIN_ID)) {
        /* already running (autostarted, or a second manual launch): nudge it to repaint
         * instead of silently doing nothing, so "open Facebook" after autostart still
         * shows signs of life. This only forces a redraw; it does not change window
         * focus (no MMK_SetFocus-style call in the API), so if another window
         * currently has focus the status screen stays hidden behind it. */
        api->MMK_PostMsg(WIN_ID, MSG_FULL_PAINT, 0, 0);
        return;
    }
    s->mem = api->alloc(MEMSZ);
    if (!s->mem)
        return;
    zero(s->mem, MEMSZ);
    uint32_t a = ((uint32_t)s->mem + 31u) & ~31u;
    s->rx = (uint8_t *)a;
    s->txb = s->rx + RXSZ;
    s->scr = s->txb + NSLOT * TXS;
    s->t_start = now_ms();
    s->t_active = s->t_start;
    MMI_WINDOW_CREATE_T c;
    zero(&c, sizeof c);
    c.applet = api->MMK_GetCurApplet();
    c.win_id = WIN_ID;
    c.func = (void *)handler;
    /* Priority: 1 (normal) for a manually launched copy (f286dbg.bin from Apps -> Facebook
     * or over USB). The autostart copy (f286dbg_embed.bin) is built with F286_PRIORITY=0
     * (lowest/idle): with priority 1 it steals focus from the idle screen and swallows
     * the menu keys meant for it (seen in the emulator); 0 does not. */
#ifndef F286_PRIORITY
#define F286_PRIORITY 1u
#endif
    c.priority = F286_PRIORITY;
    api->MMK_CreateWindow(&c);
}
