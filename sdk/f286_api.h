/* f286_api.h - ABI between the in-image boot stub and binaries loaded from the SD card.
 *
 * A loaded binary is a flat, position-independent Thumb image whose first word is
 * F286_APP_MAGIC and second word is the offset of its entry function. The stub calls
 *     entry(const f286_api_t *api, void *base, uint32_t size)
 * from the MMI task. The binary must return quickly; long work goes through window timers.
 * The table only ever grows: new members are appended and `version` is bumped. */
#ifndef F286_API_H
#define F286_API_H

#include <stdint.h>

#define F286_APP_MAGIC  0x50414632u   /* "2FAP" */
#define F286_API_VERSION 3u

typedef uint32_t MMI_HANDLE_T;
typedef uint16_t GUI_COLOR_T;                       /* RGB565 */
typedef struct { int16_t left, top, right, bottom; } GUI_RECT_T;
typedef struct { uint32_t lcd_id, block_id; } GUI_LCD_DEV_INFO;
typedef struct { uint16_t *wstr_ptr; uint16_t wstr_len; } MMI_STRING_T;

/* GUISTR_STYLE_T: 0x2c bytes; only the leading fields are known (from stock callers) */
typedef struct {
    uint32_t font;          /* +0x00 font size, e.g. 16 or 20 */
    uint16_t font_color;    /* +0x04 RGB565 */
    uint16_t pad0;
    uint32_t align;         /* +0x08 stock uses 5/6/7 */
    uint8_t  rest[0x2c - 0x0c];
} GUISTR_STYLE_T;

/* MMK_CreateWindow argument, 0x2c bytes (layout from MAIN_OpenIdleWin and 30 other callers) */
typedef struct {
    MMI_HANDLE_T applet;    /* +0x00 MMK_GetCurApplet() */
    MMI_HANDLE_T parent;    /* +0x04 0 = none */
    uint32_t win_id;        /* +0x08 unique id */
    void    *func;          /* +0x0c handler, Thumb bit set */
    void    *add_data;      /* +0x10 */
    uint32_t unk14;
    uint32_t priority;      /* +0x18 0 = lowest (idle), 1 = normal */
    uint16_t unk1c;
    uint16_t pad1e;
    uint32_t style;         /* +0x20 0 = plain */
    uint32_t move_style;    /* +0x24 */
    uint32_t unk28;
} MMI_WINDOW_CREATE_T;

/* GUILIST_ITEM_T (0x1c bytes) and its data block (0x6c bytes), layout from stock callers */
typedef struct {
    uint32_t item_style;    /* +0x00 0 */
    uint32_t style_ptr;     /* +0x04 api->GUILIST_GetStyle(F286_LIST_STYLE_TEXT) */
    void    *data;          /* +0x08 GUILIST_ITEM_DATA_T * (the list copies the text) */
    uint32_t flags;         /* +0x0c 0 (bit 7 of the byte at +0x0c = group start) */
    uint16_t first_letter, item_group, group_idx, pad;
    uint32_t user_data;
} GUILIST_ITEM_T;
typedef struct {
    struct {
        const void *ptr_or_id;   /* wstr ptr (TEXT_BUFFER) or text id (TEXT_ID) */
        uint16_t len;            /* +4 */
        uint16_t pad6;
        uint32_t pad8;
        uint8_t  type;           /* +0x0c: 1 TEXT_ID, 2 TEXT_BUFFER */
        uint8_t  pad[3];
    } content[6];
    uint32_t softkey_id[3];      /* +0x60: left, middle, right text ids (shown while this item is focused) */
} GUILIST_ITEM_DATA_T;

/* window table codes (this firmware's MMK_PARSE_WIN_E, differs from the Mocor guide) */
#define F286_WT_FUNC      0xf001u  /* WIN_FUNC(handler)         */
#define F286_WT_ID        0xf002u  /* WIN_ID(id)                */
#define F286_WT_TEXT      0xf008u  /* CREATE_TEXT_CTRL(id)      */
#define F286_WT_LISTBOX   0xf009u  /* CREATE_LISTBOX_CTRL(GUILIST_TEXTLIST_E=3, id) */
#define F286_WT_EDIT_TEXT 0xf00au  /* CREATE_EDIT_TEXT_CTRL(max_len, id) */
#define F286_WT_EDIT_PHONENUM 0xf00bu  /* CREATE_EDIT_PHONENUM_CTRL(max_len, id): "123" input, keys type digits and * (emulator-tested) */
#define F286_WT_EDIT_DIGITAL 0xf00cu   /* CREATE_EDIT_DIGITAL_CTRL(max_len, id): digits (next value in the enum; lightly tested) */
#define F286_WT_STATUSBAR 0xf01fu  /* WIN_STATUSBAR            */
#define F286_WT_SOFTKEY   0xf020u  /* WIN_SOFTKEY(left, mid, right text ids) */
#define F286_WT_TITLE     0xf021u  /* WIN_TITLE(text id)        */
#define F286_WT_END       0xf030u  /* END_WIN                   */
#define F286_LIST_TEXT    3u       /* GUILIST_TEXTLIST_E */
#define F286_LIST_STYLE_TEXT 6u    /* one-line text item style */

/* stock text ids (resolved in the emulator with api->MMI_GetLabelText) */
#define F286_TXT_NULL    0x40000001u
#define F286_TXT_MENU    0x4000000eu
#define F286_TXT_EXIT    0x4000000du
#define F286_TXT_SELECT  0x40000010u
#define F286_TXT_OK      0x40000011u
#define F286_TXT_BACK    0x40000012u
#define F286_TXT_CANCEL  0x40000015u
#define F286_TXT_DONE    0x40000016u
#define F286_TXT_SAVE    0x40000017u
#define F286_TXT_OPTIONS 0x40000019u
#define F286_TXT_DELETE  0x4000001cu
#define F286_TXT_SEARCH  0x4000001du
#define F286_TXT_EDIT    0x400000c1u
#define F286_TXT_ADD     0x400000c7u
#define F286_TXT_VIEW    0x400000c8u
/* alert images: 0x400000f1 and 0x400000f4 seen at stock call sites (warning-style box) */

/* window messages seen by a table window's handler */
#define MSG_APP_OK      0xe001u    /* left softkey */
#define MSG_APP_CANCEL  0xe002u    /* right softkey / back */
#define MSG_APP_WEB     0xe004u    /* centre OK key */

typedef uint32_t (*MMI_WIN_FUNC)(uint32_t win_id, uint32_t msg, void *param);

/* message ids (docs/api-addresses.md) */
#define MSG_TIMER          0xf020u
#define MSG_OPEN_WINDOW    0xf021u
#define MSG_CLOSE_WINDOW   0xf022u   /* likely */
#define MSG_LOSE_FOCUS     0xf023u
#define MSG_FULL_PAINT     0xf025u
#define KEY_PRESSED        0xfa00u
#define KEY_RELEASED       0xfb00u
#define KEY_LONG_PRESSED   0xfc00u
enum {
    KEY_UP = 0x01, KEY_DOWN = 0x02, KEY_LEFT = 0x03, KEY_RIGHT = 0x04, KEY_GREEN = 0x05,
    KEY_1 = 0x08, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_STAR = 0x11, KEY_0 = 0x12, KEY_HASH = 0x13,
    KEY_LSOFT = 0x16, KEY_RSOFT = 0x17, KEY_RED = 0x18, KEY_OK = 0x1b,
};

typedef struct f286_api {
    uint32_t version;
    uint32_t size;          /* sizeof(f286_api_t) as built into the stub */
    /* window manager */
    MMI_HANDLE_T (*MMK_GetCurApplet)(void);
    MMI_HANDLE_T (*MMK_CreateWindow)(const MMI_WINDOW_CREATE_T *create);
    uint32_t (*MMK_CloseWin)(MMI_HANDLE_T win);
    uint32_t (*MMK_IsOpenWin)(MMI_HANDLE_T win);
    uint32_t (*MMK_SendMsg)(MMI_HANDLE_T h, uint32_t msg, void *param);
    uint32_t (*MMK_PostMsg)(MMI_HANDLE_T h, uint32_t msg, void *param, uint32_t size);
    /* One-shot window timer: (win, ms, 0, 0, 0), re-arm from MSG_TIMER. A nonzero third
     * arg selects the "MMK_period_timer" path, which stock never uses and which resets
     * the phone. */
    uint8_t  (*MMK_CreateWinTimer)(MMI_HANDLE_T win, uint32_t ms, uint32_t must_be_0,
                                   uint32_t zero3, uint32_t zero4);
    uint32_t (*MMK_StopTimer)(uint8_t timer_id);
    /* memory: wrappers that supply the stock __FILE__/__LINE__ arguments */
    void    *(*alloc)(uint32_t size);
    void     (*free)(void *p);
    /* SFS, UCS-2 paths ("E:\\..." is the SD card) */
    uint32_t (*SFS_CreateFile)(const uint16_t *path, uint32_t mode, uint32_t share, uint32_t attr);
    uint32_t (*SFS_ReadFile)(uint32_t h, void *buf, uint32_t len, uint32_t *read, void *ov);
    uint32_t (*SFS_WriteFile)(uint32_t h, const void *buf, uint32_t len, uint32_t *written, void *ov);
    uint32_t (*SFS_CloseFile)(uint32_t h);
    uint32_t (*SFS_GetFileSize)(uint32_t h, uint32_t *size);
    /* drawing */
    void (*GUI_FillRect)(const GUI_LCD_DEV_INFO *dev, GUI_RECT_T rect, GUI_COLOR_T color);
    void (*GUISTR_DrawTextToLCDInRect)(const GUI_LCD_DEV_INFO *dev, const GUI_RECT_T *disp,
                                       const GUI_RECT_T *clip, const MMI_STRING_T *str,
                                       GUISTR_STYLE_T *style, uint32_t state, int dir);
    /* load and run another SD binary through the same path as the launcher */
    int32_t (*run_file)(const uint16_t *path);
    /* v2: debug breadcrumb, creates the empty file D:\F286_<step>_<value>.TXT
     * (survives a reset; visible in the phone's file manager) */
    void (*crumb)(uint32_t step, uint32_t value);

    /* ---- v3: stock UI controls (check api->version >= 3; see f286_ui.h for helpers) ----
     * Windows are built from a window table (array of uint32, see F286_WT_* below) and
     * opened with MMK_CreateWin; the stock window manager then creates and lays out the
     * title bar, softkey bar and controls. ctrl ids/win ids are plain uint32 you choose
     * (e.g. 0x00f28xxx). Text ids (F286_TXT_*) name stock strings; custom text goes
     * through MMI_STRING_T / (wstr,len) arguments. */
    MMI_HANDLE_T (*MMK_CreateWin)(const uint32_t *win_table, void *add_data);
    void (*MMK_SetAtvCtrl)(MMI_HANDLE_T win, uint32_t ctrl_id);   /* focus a control */
    /* list box */
    void (*GUILIST_SetMaxItem)(uint32_t ctrl, uint32_t max_items, uint32_t dynamic /*0*/);
    void (*GUILIST_AppendItem)(uint32_t ctrl, const GUILIST_ITEM_T *item);
    uint32_t (*GUILIST_GetCurItemIndex)(uint32_t ctrl);
    void (*GUILIST_SetCurItemIndex)(uint32_t ctrl, uint32_t index);
    uint32_t (*GUILIST_RemoveAllItems)(uint32_t ctrl);
    uint32_t (*GUILIST_GetStyle)(uint32_t style_enum);            /* theme style pointer for item[1] */
    /* title / softkeys of a window */
    uint32_t (*GUIWIN_SetSoftkeyTextId)(MMI_HANDLE_T win, uint32_t text_id, uint32_t button /*0 L,1 M,2 R*/,
                                        uint32_t update);
    void (*GUIWIN_SetTitleTextId)(MMI_HANDLE_T win, uint32_t text_id); /* only for windows made without F286_WT_TITLE */
    uint32_t (*GUIWIN_SetSubTitleText)(MMI_HANDLE_T win, uint32_t has_text, const uint16_t *wstr,
                                       uint32_t len, uint32_t update); /* short text in the right-hand tab */
    /* label, text box, edit box */
    uint32_t (*GUILABEL_SetText)(uint32_t ctrl, const MMI_STRING_T *str, uint32_t update);
    uint32_t (*GUITEXT_SetString)(uint32_t ctrl, const uint16_t *wstr, uint32_t len, uint32_t update);
    void (*GUIEDIT_GetString)(uint32_t ctrl, MMI_STRING_T *out);   /* points into the control, copy it */
    uint32_t (*GUIEDIT_SetString)(uint32_t ctrl, const uint16_t *wstr, uint32_t len);
    uint32_t (*GUIEDIT_ClearAllStr)(uint32_t ctrl);
    /* message box (modal, auto-closes on its softkey/timeout). time_ms may be NULL. */
    void (*MMIPUB_OpenAlertWinByTextPtr)(const uint32_t *time_ms, const MMI_STRING_T *text1,
                                         const MMI_STRING_T *text2, uint32_t image_id,
                                         const uint32_t *win_id, const uint32_t *prio,
                                         uint32_t softkey_style, void *handle_func);
    void (*MMIPUB_OpenAlertWinByTextId)(const uint32_t *time_ms, uint32_t text1_id, uint32_t text2_id,
                                        uint32_t image_id, const uint32_t *win_id, const uint32_t *prio,
                                        uint32_t softkey_style, void *handle_func);
    uint32_t (*MMI_GetLabelText)(uint32_t text_id, uint32_t applet, MMI_STRING_T *out);
} f286_api_t;

#define SFS_MODE_READ_EXISTING 0x31u    /* OPEN_EXISTING | READ, the stock default */
#define SFS_MODE_CREATE_ALWAYS_WRITE 0x22u   /* CREATE_ALWAYS | WRITE: create/overwrite */

typedef void (*f286_entry_t)(const f286_api_t *api, void *base, uint32_t size);

#endif
