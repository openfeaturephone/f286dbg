/* deadman.c - dead-man switch for unattended phone work. Uploaded and run over USB
 * (f286.py deadman on); lives in RAM until the next reset.
 *
 * Every 1 s it re-arms the SC2720 PMIC watchdog through the stock kernel driver
 * 0x8136f3f0(cfg{run 1, mode 0 = reset, load}): each call rewrites the load, which restarts
 * the count. If the MMI task stops (a hang, e.g. the pager livelock a bad pointer causes),
 * the feeding stops and the PMIC resets the phone after DEADMAN_S seconds; the boot
 * autostart brings the USB agent back. Closing the window disarms it (run 0 clears RUN).
 * The same driver call is the phone-verified remote reboot (f286.py reboot).
 *
 * DO NOT USE UNATTENDED. Left armed for about 10 minutes it once left my phone with no
 * serial link on any later boot until the battery was pulled; the cause is not known
 * (README, "Dead-man incident"). */
#include "f286_api.h"
#include "app_rt.h"

F286_APP_HEADER(f286_main);

#define WIN_ID      0x00f28640u
#define TICK_MS     1000u
#define DEADMAN_S   30u
#define WDG_CONFIG  ((uint32_t (*)(const uint32_t *))(0x8136f3f0u | 1u))

typedef struct {
    const f286_api_t *api;
    uint8_t timer;
    uint32_t feeds;
} state_t;
APP_STATE(state_t);

static void wdg(uint32_t run)
{
    uint32_t cfg[3];
    cfg[0] = run;
    cfg[1] = 0;                         /* mode 0: interrupt + reset */
    cfg[2] = DEADMAN_S * 32768u;        /* 32 kHz counter */
    WDG_CONFIG(cfg);
}

static uint32_t handler(uint32_t win_id, uint32_t msg, void *param)
{
    state_t *s = STATE();
    const f286_api_t *api = s->api;
    switch (msg) {
    case MSG_OPEN_WINDOW:
        wdg(1);
        s->timer = api->MMK_CreateWinTimer(win_id, TICK_MS, 0, 0, 0);
        return 1;
    case MSG_TIMER:
        if (param && *(uint8_t *)param == s->timer) {
            wdg(1);
            s->feeds++;
            s->timer = api->MMK_CreateWinTimer(win_id, TICK_MS, 0, 0, 0);
            return 1;
        }
        return 0;
    case MSG_CLOSE_WINDOW:
        if (s->timer)
            api->MMK_StopTimer(s->timer);
        s->timer = 0;
        wdg(0);
        return 1;
    case MSG_FULL_PAINT:
        return 1;                       /* lowest priority, never on top: draw nothing */
    default:
        return 0;
    }
}

void f286_main(const f286_api_t *api, void *base, uint32_t size)
{
    (void)base;
    (void)size;
    if (api->version < 2 || api->MMK_IsOpenWin(WIN_ID))
        return;
    state_t *s = STATE();
    zero(s, sizeof *s);
    s->api = api;
    MMI_WINDOW_CREATE_T c;
    zero(&c, sizeof c);
    c.applet = api->MMK_GetCurApplet();
    c.win_id = WIN_ID;
    c.func = (void *)handler;
    c.priority = 0;
    api->MMK_CreateWindow(&c);
}
