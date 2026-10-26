// user32: menus (data model, resource templates, the menu bar of a top-level window, popup menus, accelerators)
//
// A menu bar is painted by user32 into the top of the window's buffer (WS_CAPTION windows have their title bar from the desktop;
// the client area starts mh pixels lower). Popup menus are small frameless /dev/okwin windows with their own pixel buffer. While
// a menu is open its modal loop (menu_run) receives the events of the connection through U_event_hook.
#include "u32.h"

extern HRSRC FindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang);
extern HGLOBAL LoadResource(HMODULE m, HRSRC r);
extern BOOL PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp);
extern BOOL GetCursorPos(POINT *p);

#define MF_GRAYED 1
#define MF_DISABLED 2
#define MF_CHECKED 8
#define MF_POPUP 0x10
#define MF_MENUBARBREAK 0x20
#define MF_MENUBREAK 0x40
#define MF_END 0x80
#define MF_BYPOSITION 0x400
#define MF_SEPARATOR 0x800
#define MF_HELP 0x4000
#define MF_OWNERDRAW 0x100
#define MF_BITMAP 4
#define MF_DEFAULT 0x1000
#define MFS_GRAYED 3
#define MFS_CHECKED 8
#define MFS_DEFAULT 0x1000
#define MFT_SEPARATOR 0x800
#define MFT_RADIOCHECK 0x200
#define MFT_STRING 0
#define MIIM_STATE 1
#define MIIM_ID 2
#define MIIM_SUBMENU 4
#define MIIM_CHECKMARKS 8
#define MIIM_TYPE 0x10
#define MIIM_DATA 0x20
#define MIIM_STRING 0x40
#define MIIM_FTYPE 0x100
#define TPM_RETURNCMD 0x100
#define TPM_NONOTIFY 0x80
#define TPM_CENTERALIGN 4
#define TPM_RIGHTALIGN 8
#define TPM_VCENTERALIGN 0x10
#define TPM_BOTTOMALIGN 0x20
#define WM_INITMENU 0x116
#define WM_INITMENUPOPUP 0x117
#define WM_MENUSELECT 0x11F

struct mitem {
    UINT id, flags;                       // flags: MF_* (MF_POPUP: sub is the submenu; MF_SEPARATOR; MF_GRAYED/MF_DISABLED; MF_CHECKED)
    char text[96];                        // UTF-8, '&' marks the mnemonic, '\t' separates the accelerator text
    struct menu *sub;
    ULONG_PTR data;
    int x, w;                             // menu bar layout
};
struct menu { int magic; int n, cap; struct mitem *it; int popup; };
#define MENU_MAGIC 0x4D454E55

struct menu *U_menu(HMENU h) { struct menu *m = h; return (m && m->magic == MENU_MAGIC) ? m : 0; }

static struct menu *menu_new(int popup) {
    struct menu *m = umalloc(sizeof *m);
    if (m) { memset(m, 0, sizeof *m); m->magic = MENU_MAGIC; m->popup = popup; }
    return m;
}
EXPORT HMENU CreateMenu(void) { return menu_new(0); }
EXPORT HMENU CreatePopupMenu(void) { return menu_new(1); }
EXPORT BOOL IsMenu(HMENU h) { return U_menu(h) != 0; }
EXPORT BOOL DestroyMenu(HMENU h) {
    struct menu *m = U_menu(h);
    if (!m) return FALSE;
    for (int i = 0; i < m->n; i++) if (m->it[i].sub) DestroyMenu(m->it[i].sub);
    ufree(m->it); m->magic = 0; ufree(m);
    return TRUE;
}

static int menu_insert(struct menu *m, int pos, UINT flags, UINT_PTR id, const char *text) {
    if (pos < 0 || pos > m->n) pos = m->n;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        struct mitem *ni = umalloc((SIZE_T)nc * sizeof *ni);
        if (!ni) return 0;
        if (m->it) { memcpy(ni, m->it, (SIZE_T)m->n * sizeof *ni); ufree(m->it); }
        m->it = ni; m->cap = nc;
    }
    for (int i = m->n; i > pos; i--) m->it[i] = m->it[i - 1];
    struct mitem *t = &m->it[pos];
    memset(t, 0, sizeof *t);
    t->flags = flags;
    if (flags & MF_POPUP) t->sub = U_menu((HMENU)id); else t->id = (UINT)id;
    if (!(flags & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) && text) ustrcpy(t->text, text, sizeof t->text);
    m->n++;
    return 1;
}
static const char *wide_text(const WCHAR *w, char *buf, int cap) { WideCharToMultiByte(65001, 0, w, -1, buf, cap, 0, 0); return buf; }

EXPORT BOOL AppendMenuA(HMENU h, UINT f, UINT_PTR id, LPCSTR s) { struct menu *m = U_menu(h); return m && menu_insert(m, -1, f, id, s); }
EXPORT BOOL AppendMenuW(HMENU h, UINT f, UINT_PTR id, LPCWSTR s) { char b[96]; return AppendMenuA(h, f, id, (f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) || !s ? 0 : wide_text(s, b, sizeof b)); }
EXPORT BOOL InsertMenuA(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCSTR s) {
    struct menu *m = U_menu(h);
    if (!m) return FALSE;
    return menu_insert(m, (f & MF_BYPOSITION) && pos != 0xFFFFFFFFu ? (int)pos : -1, f, id, s);
}
EXPORT BOOL InsertMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s) { char b[96]; return InsertMenuA(h, pos, f, id, (f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) || !s ? 0 : wide_text(s, b, sizeof b)); }

// the item addressed by position or by command id (searching submenus); *owner = the menu that holds it
static struct mitem *find_item(struct menu *m, UINT id, UINT flags, struct menu **owner) {
    if (flags & MF_BYPOSITION) { if (id < (UINT)m->n) { if (owner) *owner = m; return &m->it[id]; } return 0; }
    for (int i = 0; i < m->n; i++) {
        if (!(m->it[i].flags & MF_POPUP) && !(m->it[i].flags & MF_SEPARATOR) && m->it[i].id == id) { if (owner) *owner = m; return &m->it[i]; }
        if (m->it[i].sub) { struct mitem *r = find_item(m->it[i].sub, id, flags, owner); if (r) return r; }
    }
    return 0;
}
EXPORT BOOL RemoveMenu(HMENU h, UINT id, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return FALSE;
    int i = (int)(t - o->it);
    for (; i + 1 < o->n; i++) o->it[i] = o->it[i + 1];
    o->n--;
    return TRUE;
}
EXPORT BOOL DeleteMenu(HMENU h, UINT id, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return FALSE;
    if (t->sub) DestroyMenu(t->sub);
    return RemoveMenu(h, id, f);
}
EXPORT BOOL ModifyMenuA(HMENU h, UINT id, UINT f, UINT_PTR nid, LPCSTR s) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return FALSE;
    t->flags = f & ~MF_BYPOSITION;
    if (f & MF_POPUP) t->sub = U_menu((HMENU)nid); else { t->id = (UINT)nid; t->sub = 0; }
    if (!(f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) && s) ustrcpy(t->text, s, sizeof t->text);
    return TRUE;
}
EXPORT BOOL ModifyMenuW(HMENU h, UINT id, UINT f, UINT_PTR nid, LPCWSTR s) { char b[96]; return ModifyMenuA(h, id, f, nid, (f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) || !s ? 0 : wide_text(s, b, sizeof b)); }
EXPORT DWORD CheckMenuItem(HMENU h, UINT id, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return (DWORD)-1;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return (DWORD)-1;
    DWORD was = (t->flags & MF_CHECKED) ? MF_CHECKED : 0;
    if (f & MF_CHECKED) t->flags |= MF_CHECKED; else t->flags &= ~MF_CHECKED;
    return was;
}
EXPORT BOOL EnableMenuItem(HMENU h, UINT id, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return (BOOL)-1;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return (BOOL)-1;
    BOOL was = t->flags & (MF_GRAYED | MF_DISABLED);
    t->flags = (t->flags & ~(MF_GRAYED | MF_DISABLED)) | (f & (MF_GRAYED | MF_DISABLED));
    return was;
}
EXPORT BOOL CheckMenuRadioItem(HMENU h, UINT first, UINT last, UINT check, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    for (UINT i = first; i <= last; i++) { struct mitem *t = find_item(m, i, f, &o); if (t) { if (i == check) t->flags |= MF_CHECKED; else t->flags &= ~MF_CHECKED; } }
    return TRUE;
}
EXPORT int GetMenuItemCount(HMENU h) { struct menu *m = U_menu(h); return m ? m->n : -1; }
EXPORT UINT GetMenuItemID(HMENU h, int pos) { struct menu *m = U_menu(h); if (!m || pos < 0 || pos >= m->n) return (UINT)-1; return m->it[pos].sub ? (UINT)-1 : m->it[pos].id; }
EXPORT HMENU GetSubMenu(HMENU h, int pos) { struct menu *m = U_menu(h); if (!m || pos < 0 || pos >= m->n) return 0; return m->it[pos].sub; }
EXPORT UINT GetMenuState(HMENU h, UINT id, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return (UINT)-1;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return (UINT)-1;
    UINT r = t->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_SEPARATOR | MF_POPUP);
    if (t->sub) r |= (UINT)t->sub->n << 8;
    return r;
}
EXPORT int GetMenuStringA(HMENU h, UINT id, LPSTR out, int cap, UINT f) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return 0;
    struct mitem *t = find_item(m, id, f, &o);
    if (!t) return 0;
    if (!cap || !out) return ustrlen(t->text);
    ustrcpy(out, t->text, cap);
    return ustrlen(out);
}
EXPORT int GetMenuStringW(HMENU h, UINT id, LPWSTR out, int cap, UINT f) {
    char b[96]; int n = GetMenuStringA(h, id, b, sizeof b, f);
    if (!cap || !out) return n;
    MultiByteToWideChar(65001, 0, b, -1, out, cap);
    return n;
}
EXPORT BOOL SetMenuDefaultItem(HMENU h, UINT id, UINT byp) { (void)h; (void)id; (void)byp; return TRUE; }
EXPORT UINT GetMenuDefaultItem(HMENU h, UINT byp, UINT fl) { (void)h; (void)byp; (void)fl; return (UINT)-1; }

typedef struct { UINT cbSize, fMask, fType, fState, wID; HMENU hSubMenu; HBITMAP hbmpChecked, hbmpUnchecked; ULONG_PTR dwItemData; char *dwTypeData; UINT cch; HBITMAP hbmpItem; } MENUITEMINFOA;
static void fill_item(struct mitem *t, const MENUITEMINFOA *mi, int wide) {
    if (mi->fMask & MIIM_ID) t->id = mi->wID;
    if (mi->fMask & MIIM_SUBMENU) { t->sub = U_menu(mi->hSubMenu); if (t->sub) t->flags |= MF_POPUP; else t->flags &= ~MF_POPUP; }
    if (mi->fMask & MIIM_STATE) t->flags = (t->flags & ~(MF_GRAYED | MF_DISABLED | MF_CHECKED)) | ((mi->fState & MFS_GRAYED) ? MF_GRAYED : 0) | ((mi->fState & MFS_CHECKED) ? MF_CHECKED : 0);
    if (mi->fMask & (MIIM_TYPE | MIIM_FTYPE)) { if (mi->fType & MFT_SEPARATOR) t->flags |= MF_SEPARATOR; else t->flags &= ~MF_SEPARATOR; }
    if (mi->fMask & MIIM_DATA) t->data = mi->dwItemData;
    if ((mi->fMask & (MIIM_STRING | MIIM_TYPE)) && mi->dwTypeData && !(t->flags & MF_SEPARATOR)) {
        if (wide) { char b[96]; ustrcpy(t->text, wide_text((const WCHAR*)mi->dwTypeData, b, sizeof b), sizeof t->text); }
        else ustrcpy(t->text, mi->dwTypeData, sizeof t->text);
    }
}
static BOOL insert_mii(HMENU h, UINT pos, BOOL byp, const MENUITEMINFOA *mi, int wide) {
    struct menu *m = U_menu(h);
    if (!m) return FALSE;
    int at = byp ? (int)pos : -1;
    if (!menu_insert(m, at, 0, 0, 0)) return FALSE;
    struct mitem *t = &m->it[at < 0 || at > m->n - 1 ? m->n - 1 : at];
    fill_item(t, mi, wide);
    return TRUE;
}
EXPORT BOOL InsertMenuItemA(HMENU h, UINT pos, BOOL byp, const MENUITEMINFOA *mi) { return insert_mii(h, pos, byp, mi, 0); }
EXPORT BOOL InsertMenuItemW(HMENU h, UINT pos, BOOL byp, const MENUITEMINFOA *mi) { return insert_mii(h, pos, byp, mi, 1); }
EXPORT BOOL SetMenuItemInfoA(HMENU h, UINT id, BOOL byp, const MENUITEMINFOA *mi) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    struct mitem *t = find_item(m, id, byp ? MF_BYPOSITION : 0, &o);
    if (!t) return FALSE;
    fill_item(t, mi, 0);
    return TRUE;
}
EXPORT BOOL SetMenuItemInfoW(HMENU h, UINT id, BOOL byp, const MENUITEMINFOA *mi) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    struct mitem *t = find_item(m, id, byp ? MF_BYPOSITION : 0, &o);
    if (!t) return FALSE;
    fill_item(t, mi, 1);
    return TRUE;
}
static BOOL get_mii(HMENU h, UINT id, BOOL byp, MENUITEMINFOA *mi, int wide) {
    struct menu *m = U_menu(h), *o = 0;
    if (!m) return FALSE;
    struct mitem *t = find_item(m, id, byp ? MF_BYPOSITION : 0, &o);
    if (!t) return FALSE;
    if (mi->fMask & MIIM_ID) mi->wID = t->id;
    if (mi->fMask & MIIM_SUBMENU) mi->hSubMenu = t->sub;
    if (mi->fMask & MIIM_STATE) mi->fState = ((t->flags & (MF_GRAYED | MF_DISABLED)) ? MFS_GRAYED : 0) | ((t->flags & MF_CHECKED) ? MFS_CHECKED : 0);
    if (mi->fMask & (MIIM_TYPE | MIIM_FTYPE)) mi->fType = (t->flags & MF_SEPARATOR) ? MFT_SEPARATOR : 0;
    if (mi->fMask & MIIM_DATA) mi->dwItemData = t->data;
    if ((mi->fMask & (MIIM_STRING | MIIM_TYPE)) && mi->dwTypeData && mi->cch) {
        if (wide) MultiByteToWideChar(65001, 0, t->text, -1, (LPWSTR)mi->dwTypeData, (int)mi->cch); else ustrcpy(mi->dwTypeData, t->text, (int)mi->cch);
    }
    mi->cch = (UINT)ustrlen(t->text);
    return TRUE;
}
EXPORT BOOL GetMenuItemInfoA(HMENU h, UINT id, BOOL byp, MENUITEMINFOA *mi) { return get_mii(h, id, byp, mi, 0); }
EXPORT BOOL GetMenuItemInfoW(HMENU h, UINT id, BOOL byp, MENUITEMINFOA *mi) { return get_mii(h, id, byp, mi, 1); }

static void bar_layout(struct wnd *w, HDC dc);

// ---- menu templates in resources ------------------------------------------------------------------------------------------------

static const BYTE *tmpl_text(const BYTE *p, char *out, int cap) {                 // a NUL terminated UTF-16 string
    const WCHAR *w = (const WCHAR*)p;
    int n = 0; while (w[n]) n++;
    WideCharToMultiByte(65001, 0, w, n + 1, out, cap, 0, 0);
    return (const BYTE*)(w + n + 1);
}
// old format: {WORD option; [WORD id]; text} items, MF_POPUP ones are followed by their items, MF_END ends a list
static const BYTE *parse_old(const BYTE *p, struct menu *m) {
    for (;;) {
        WORD opt = *(const WORD*)p; p += 2;
        char text[96];
        if (opt & MF_POPUP) {
            p = tmpl_text(p, text, sizeof text);
            struct menu *sub = menu_new(1);
            p = parse_old(p, sub);
            menu_insert(m, -1, opt & ~(MF_END), (UINT_PTR)sub, text);
        } else {
            WORD id = *(const WORD*)p; p += 2;
            p = tmpl_text(p, text, sizeof text);
            menu_insert(m, -1, opt & ~MF_END, id, (id == 0 && text[0] == 0) ? 0 : text);
            if (id == 0 && text[0] == 0) m->it[m->n - 1].flags |= MF_SEPARATOR;
        }
        if (opt & MF_END) return p;
    }
}
// extended format (MENUEX): {DWORD type, state; DWORD id; WORD flags; text; [DWORD helpId if popup]}, flags 1 = popup, 0x80 = last
static const BYTE *parse_ex(const BYTE *p, struct menu *m) {
    for (;;) {
        DWORD type = *(const DWORD*)p, state = *(const DWORD*)(p + 4), id = *(const DWORD*)(p + 8);
        WORD fl = *(const WORD*)(p + 12);
        p += 14;
        char text[96];
        p = tmpl_text(p, text, sizeof text);
        p = (const BYTE*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
        UINT f = 0;
        if (state & MFS_GRAYED) f |= MF_GRAYED;
        if (state & MFS_CHECKED) f |= MF_CHECKED;
        if (type & MFT_SEPARATOR) f |= MF_SEPARATOR;
        if (fl & 1) {
            p += 4;                                                                // help id
            struct menu *sub = menu_new(1);
            p = parse_ex(p, sub);
            menu_insert(m, -1, f | MF_POPUP, (UINT_PTR)sub, text);
        } else menu_insert(m, -1, f, id, (type & MFT_SEPARATOR) ? 0 : text);
        if (fl & 0x80) return p;
    }
}
EXPORT HMENU LoadMenuIndirectW(const void *tmpl) {
    const BYTE *p = tmpl;
    WORD ver = *(const WORD*)p, hdr = *(const WORD*)(p + 2);
    struct menu *m = menu_new(0);
    if (!m) return 0;
    if (ver == 0) parse_old(p + 4 + hdr, m);
    else {
        p += 4 + hdr;
        p = (const BYTE*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
        parse_ex(p, m);
    }
    return m;
}
EXPORT HMENU LoadMenuIndirectA(const void *tmpl) { return LoadMenuIndirectW(tmpl); }
EXPORT HMENU LoadMenuW(HINSTANCE h, LPCWSTR name) {
    HRSRC r = FindResourceExW(h, (LPCWSTR)(ULONG_PTR)4, name, 0);                   // RT_MENU
    if (!r) return 0;
    return LoadMenuIndirectW(LoadResource(h, r));
}
EXPORT HMENU LoadMenuA(HINSTANCE h, LPCSTR name) {
    WCHAR w[64];
    if ((ULONG_PTR)name < 0x10000) return LoadMenuW(h, (LPCWSTR)name);
    MultiByteToWideChar(65001, 0, name, -1, w, 64);
    return LoadMenuW(h, w);
}

EXPORT BOOL SetMenu(HWND hw, HMENU hm) {
    struct wnd *w = U_wnd(hw);
    if (!w || w->parent) return FALSE;
    struct menu *m = U_menu(hm);
    int mh = m ? U_menubar_height() : 0;
    int old = w->mh;
    w->menu = m; w->mh = mh;
    if (mh != old) {                                              // the buffer stays, the client area gets smaller or larger
        w->ch = w->bh - mh;
        U_send(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->cw, w->ch));
        U_invalidate(w, 0, 1);
    }
    U_menubar_paint(w);
    return TRUE;
}
EXPORT HMENU GetMenu(HWND hw) { struct wnd *w = U_wnd(hw); return w && !w->parent ? w->menu : 0; }
EXPORT BOOL DrawMenuBar(HWND hw) { struct wnd *w = U_wnd(hw); if (!w || w->parent) return FALSE; U_menubar_paint(w); return TRUE; }
EXPORT HMENU GetSystemMenu(HWND hw, BOOL revert) { (void)hw; (void)revert; return 0; }
EXPORT BOOL HiliteMenuItem(HWND hw, HMENU m, UINT id, UINT f) { (void)hw; (void)m; (void)id; (void)f; return TRUE; }
EXPORT int MenuItemFromPoint(HWND hw, HMENU m, POINT p) { (void)hw; (void)m; (void)p; return -1; }
EXPORT BOOL GetMenuItemRect(HWND hw, HMENU hm, UINT pos, RECT *r) {       // items of a window's menu bar, screen coordinates
    struct wnd *w = U_wnd(hw);
    struct menu *m = U_menu(hm);
    r->left = r->top = r->right = r->bottom = 0;
    if (!w || !m || w->menu != m || pos >= (UINT)m->n) return FALSE;
    HDC dc = __gdi_make_dc(0, 1, 1, 1);
    bar_layout(w, dc);
    __gdi_free_dc(dc);
    int sx, sy; U_screen_origin(w, &sx, &sy);
    r->left = sx + m->it[pos].x; r->right = r->left + m->it[pos].w; r->top = sy - w->mh; r->bottom = sy;
    return TRUE;
}

// ---- layout and painting ---------------------------------------------------------------------------------------------------------

static int bar_h;
int U_menubar_height(void) {
    if (!bar_h) { HDC dc = __gdi_make_dc(0, 1, 1, 1); bar_h = U_text_height(dc) + 10; __gdi_free_dc(dc); }
    return bar_h;
}

// text without the '&', mnemonic position (-1 none) and the accelerator part after a tab
static int split_text(const char *t, char *plain, int cap, char *accel, int acap, int *mnem) {
    int n = 0, an = 0, in_acc = 0;
    *mnem = -1;
    if (accel) accel[0] = 0;
    for (int i = 0; t[i] && n < cap - 1; i++) {
        if (t[i] == '\t') { in_acc = 1; continue; }
        if (in_acc) { if (accel && an < acap - 1) accel[an++] = t[i]; continue; }
        if (t[i] == '&') { if (t[i + 1] == '&') { plain[n++] = '&'; i++; } else if (t[i + 1]) *mnem = n; continue; }
        plain[n++] = t[i];
    }
    plain[n] = 0;
    if (accel) accel[an] = 0;
    return n;
}

static void draw_item_text(HDC dc, int x, int y, const char *plain, int mnem, COLORREF col) {
    SetTextColor(dc, col);
    TextOutA(dc, x, y, plain, ustrlen(plain));
    if (mnem >= 0) {
        int x0 = x + U_text_width(dc, plain, mnem), x1 = x + U_text_width(dc, plain, mnem + 1), th = U_text_height(dc);
        __gdi_rect_fill_color(dc, x0, y + th - 1, x1, y + th, col);
    }
}

static int hot_bar = -1;
static void bar_layout(struct wnd *w, HDC dc) {
    int x = 0;
    for (int i = 0; i < w->menu->n; i++) {
        char plain[96]; int mn;
        split_text(w->menu->it[i].text, plain, sizeof plain, 0, 0, &mn);
        w->menu->it[i].x = x;
        w->menu->it[i].w = U_text_width(dc, plain, ustrlen(plain)) + 20;
        x += w->menu->it[i].w;
    }
}
void U_menubar_paint(struct wnd *w) {
    if (!w->menu || w->parent || !w->px) return;
    HDC dc = __gdi_make_dc(w->px, w->bw, w->bh, w->stride);
    if (!dc) return;
    RECT clip = { 0, 0, w->bw, w->mh };
    __gdi_dc_setup(dc, w->px, w->bw, w->bh, w->stride, 0, 0, &clip);
    SetBkMode(dc, TRANSPARENT);
    __gdi_rect_fill_color(dc, 0, 0, w->bw, w->mh, RGB(240, 240, 240));
    __gdi_rect_fill_color(dc, 0, w->mh - 1, w->bw, w->mh, RGB(200, 200, 200));
    bar_layout(w, dc);
    int th = U_text_height(dc);
    for (int i = 0; i < w->menu->n; i++) {
        struct mitem *t = &w->menu->it[i];
        char plain[96]; int mn;
        split_text(t->text, plain, sizeof plain, 0, 0, &mn);
        if (i == hot_bar && U_top(U_active()) == w) __gdi_rect_fill_color(dc, t->x, 1, t->x + t->w, w->mh - 1, RGB(205, 225, 245));
        draw_item_text(dc, t->x + 10, (w->mh - th) / 2, plain, mn, (t->flags & (MF_GRAYED | MF_DISABLED)) ? RGB(150, 150, 150) : RGB(0, 0, 0));
    }
    __gdi_free_dc(dc);
    struct okw_present p = { w->wid, 0, 0, w->bw, w->mh };
    SC3(LX_ioctl, U_fd(), OKW_PRESENT, &p);
}

// ---- popup menus ------------------------------------------------------------------------------------------------------------------

struct pop {
    struct menu *m;
    int wid, w, h, x, y;                  // screen position of the window's outer rectangle
    unsigned *px;
    int hot;
    int ih;
    int tw, aw;                           // widest text, widest accelerator
};
#define MAXPOP 8
static struct pop pops[MAXPOP];
static int npop;
static struct {
    int done, result, returncmd;
    struct wnd *owner;
    int bar;                              // index of the menu bar item that is open (-1 = a free standing popup)
    HMENU root;
} R;

#define SEP_H 9
static int item_h(HDC dc) { return U_text_height(dc) + 8; }

static void pop_measure(struct pop *p) {
    HDC dc = __gdi_make_dc(0, 1, 1, 1);
    p->ih = item_h(dc);
    int tw = 0, aw = 0, h = 4;
    for (int i = 0; i < p->m->n; i++) {
        struct mitem *t = &p->m->it[i];
        if (t->flags & MF_SEPARATOR) { h += SEP_H; continue; }
        char plain[96], acc[48]; int mn;
        split_text(t->text, plain, sizeof plain, acc, sizeof acc, &mn);
        int w1 = U_text_width(dc, plain, ustrlen(plain)), w2 = U_text_width(dc, acc, ustrlen(acc));
        if (w1 > tw) tw = w1;
        if (w2 > aw) aw = w2;
        h += p->ih;
    }
    __gdi_free_dc(dc);
    p->tw = tw; p->aw = aw;
    p->w = 28 + tw + (aw ? 24 + aw : 0) + 36;
    p->h = h + 4;
}
static int item_y(struct pop *p, int idx) {
    int y = 4;
    for (int i = 0; i < idx; i++) y += (p->m->it[i].flags & MF_SEPARATOR) ? SEP_H : p->ih;
    return y;
}
static int item_at(struct pop *p, int px, int py) {
    if (px < 0 || px >= p->w) return -1;
    int y = 4;
    for (int i = 0; i < p->m->n; i++) {
        int hh = (p->m->it[i].flags & MF_SEPARATOR) ? SEP_H : p->ih;
        if (py >= y && py < y + hh) return i;
        y += hh;
    }
    return -1;
}
static int selectable(struct mitem *t) { return !(t->flags & (MF_SEPARATOR | MF_GRAYED | MF_DISABLED)); }

static void pop_paint(struct pop *p) {
    HDC dc = __gdi_make_dc(p->px, p->w, p->h, p->w);
    if (!dc) return;
    RECT clip = { 0, 0, p->w, p->h };
    __gdi_dc_setup(dc, p->px, p->w, p->h, p->w, 0, 0, &clip);
    SetBkMode(dc, TRANSPARENT);
    __gdi_rect_fill_color(dc, 0, 0, p->w, p->h, RGB(248, 248, 248));
    int th = U_text_height(dc);
    for (int i = 0; i < p->m->n; i++) {
        struct mitem *t = &p->m->it[i];
        int y = item_y(p, i);
        if (t->flags & MF_SEPARATOR) { __gdi_rect_fill_color(dc, 28, y + SEP_H / 2, p->w - 6, y + SEP_H / 2 + 1, RGB(210, 210, 210)); continue; }
        int off = (t->flags & (MF_GRAYED | MF_DISABLED)) != 0;
        if (i == p->hot && !off) __gdi_rect_fill_color(dc, 3, y, p->w - 3, y + p->ih, RGB(205, 225, 245));
        char plain[96], acc[48]; int mn;
        split_text(t->text, plain, sizeof plain, acc, sizeof acc, &mn);
        COLORREF col = off ? RGB(150, 150, 150) : RGB(0, 0, 0);
        draw_item_text(dc, 28, y + (p->ih - th) / 2, plain, mn, col);
        if (acc[0]) draw_item_text(dc, p->w - 14 - p->aw - (t->sub ? 14 : 0), y + (p->ih - th) / 2, acc, -1, col);
        if (t->flags & MF_CHECKED) {                                      // a check mark in the left column
            int cx = 9, cy = y + p->ih / 2;
            for (int k = 0; k < 2; k++) {
                for (int s = 0; s < 4; s++) __gdi_rect_fill_color(dc, cx + s, cy + s - 1 + k, cx + s + 1, cy + s + k, col);
                for (int s = 0; s < 7; s++) __gdi_rect_fill_color(dc, cx + 3 + s, cy + 2 - s + k, cx + 4 + s, cy + 3 - s + k, col);
            }
        }
        if (t->sub) {                                                      // an arrow for a submenu
            int ax = p->w - 14, ay = y + p->ih / 2;
            for (int s = 0; s < 5; s++) __gdi_rect_fill_color(dc, ax + s, ay - 5 + s, ax + s + 1, ay + 5 - s, col);
        }
    }
    __gdi_free_dc(dc);
    struct okw_present pr = { p->wid, 0, 0, p->w, p->h };
    SC3(LX_ioctl, U_fd(), OKW_PRESENT, &pr);
}

static int g_minw, g_sel = -1;
static struct pop *pop_open(struct menu *m, int sx, int sy) {
    if (npop >= MAXPOP) return 0;
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    struct pop *p = &pops[npop];
    memset(p, 0, sizeof *p);
    p->m = m; p->hot = -1;
    pop_measure(p);
    if (R.owner) U_send(R.owner, WM_INITMENUPOPUP, (WPARAM)m, MAKELPARAM(0, 0));
    pop_measure(p);                                                       // the owner may have changed the items
    if (g_minw > p->w) p->w = g_minw;
    g_minw = 0;
    if (g_sel >= 0 && g_sel < m->n) p->hot = g_sel;
    g_sel = -1;
    int ow = p->w + 2 * border, oh = p->h + 2 * border;
    if (sx + ow > sw) sx = sw - ow;
    if (sy + oh > sh - 40) sy = sh - 40 - oh;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    struct okw_create c;
    memset(&c, 0, sizeof c);
    c.x = sx; c.y = sy; c.w = p->w; c.h = p->h; c.flags = OKW_F_NOFRAME;
    if (SC3(LX_ioctl, U_fd(), OKW_CREATE, &c) < 0) return 0;
    struct okw_map mp; memset(&mp, 0, sizeof mp); mp.wid = c.out_wid;
    if (SC3(LX_ioctl, U_fd(), OKW_MAP, &mp) < 0) { SC3(LX_ioctl, U_fd(), OKW_DESTROY, c.out_wid); return 0; }
    p->wid = c.out_wid; p->px = (unsigned*)mp.out_addr; p->x = sx; p->y = sy;
    npop++;
    pop_paint(p);
    return p;
}
static void pop_close_to(int depth) {                                    // keep `depth` popups
    while (npop > depth) { npop--; SC3(LX_ioctl, U_fd(), OKW_DESTROY, pops[npop].wid); }
}
static struct pop *pop_of(int wid) { for (int i = 0; i < npop; i++) if (pops[i].wid == wid) return &pops[i]; return 0; }
static int pop_index(struct pop *p) { return (int)(p - pops); }

static void set_hot(struct pop *p, int idx) {
    if (p->hot == idx) return;
    p->hot = idx;
    pop_paint(p);
}
static void open_sub(struct pop *p, int idx) {
    pop_close_to(pop_index(p) + 1);
    struct mitem *t = &p->m->it[idx];
    if (!t->sub || !selectable(t)) return;
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    // the popup's client origin is inside its frame
    pop_open(t->sub, p->x + p->w + 2 * border - 4, p->y + item_y(p, idx));
}
static void choose(struct pop *p, int idx) {
    struct mitem *t = &p->m->it[idx];
    if (!selectable(t)) return;
    if (t->sub) { open_sub(p, idx); return; }
    R.result = (int)t->id; R.done = 1;
}

static int kbd_open;                                                         // the bar menu was opened with the keyboard: the first item is hot
static void bar_open(struct wnd *T, int idx);
static int bar_item_at(struct wnd *T, int x) {
    for (int i = 0; i < T->menu->n; i++) if (x >= T->menu->it[i].x && x < T->menu->it[i].x + T->menu->it[i].w) return i;
    return -1;
}

static int key_nav(struct pop *p, int code, int is_char) {
    int n = p->m->n;
    if (!is_char) {
        if (code == 108 || code == 103) {                                   // down / up
            int dir = code == 108 ? 1 : -1, i = p->hot;
            for (int k = 0; k < n; k++) { i = (i + dir + n) % n; if (selectable(&p->m->it[i])) break; }
            if (p->hot < 0 && dir < 0) { i = n - 1; while (i > 0 && !selectable(&p->m->it[i])) i--; }
            set_hot(p, i);
            return 1;
        }
        if (code == 28 || code == 96) { if (p->hot >= 0) choose(p, p->hot); return 1; }
        if (code == 106) { if (p->hot >= 0 && p->m->it[p->hot].sub) { open_sub(p, p->hot); if (npop > pop_index(p) + 1) set_hot(&pops[pop_index(p) + 1], 0); } else if (R.bar >= 0) { int nb = (R.bar + 1) % R.owner->menu->n; bar_open(R.owner, nb); } return 1; }
        if (code == 105) { if (pop_index(p) > 0) pop_close_to(pop_index(p)); else if (R.bar >= 0) { int nb = (R.bar + R.owner->menu->n - 1) % R.owner->menu->n; bar_open(R.owner, nb); } return 1; }
        if (code == 1) { if (pop_index(p) > 0) pop_close_to(pop_index(p)); else R.done = 1; return 1; }
        return 0;
    }
    int c = code; if (c >= 'A' && c <= 'Z') c += 32;
    for (int i = 0; i < n; i++) {
        char plain[96]; int mn;
        split_text(p->m->it[i].text, plain, sizeof plain, 0, 0, &mn);
        if (mn >= 0) { int m = plain[mn]; if (m >= 'A' && m <= 'Z') m += 32; if (m == c) { set_hot(p, i); choose(p, i); return 1; } }
    }
    return 0;
}

static int menu_hook(const struct okw_event *e) {
    struct pop *p = pop_of(e->wid);
    int title, border, sw, sh;
    U_info(&title, &border, &sw, &sh);
    (void)title; (void)sw; (void)sh;
    if (p) {
        switch (e->type) {
        case OKE_MOUSE_MOVE: {
            int idx = item_at(p, e->a, e->b);
            if (idx >= 0 && selectable(&p->m->it[idx])) {
                if (p->hot != idx) { set_hot(p, idx); if (p->m->it[idx].sub) open_sub(p, idx); else pop_close_to(pop_index(p) + 1); }
            } else if (idx < 0 || (p->m->it[idx].flags & MF_SEPARATOR)) set_hot(p, -1);
            return 1;
        }
        case OKE_MOUSE_BUTTON:
            if (e->c == 1 && e->d == 1) { int idx = item_at(p, e->a, e->b); if (idx >= 0) choose(p, idx); }
            else if (e->c == 1 && e->d == 0) { int idx = item_at(p, e->a, e->b); if (idx >= 0 && p->hot == idx && !p->m->it[idx].sub) choose(p, idx); }
            return 1;
        case OKE_KEY: if (e->b == 1) return key_nav(p, e->a, 0) || 1; return 1;
        case OKE_CHAR: key_nav(p, e->a, 1); return 1;
        case OKE_WHEEL: return 1;
        default: return 1;
        }
    }
    // events of other windows: a click outside dismisses; moving over the menu bar of the owner switches menus
    if (R.owner && e->wid == R.owner->wid && R.bar >= 0 && R.owner->menu) {
        if (e->type == OKE_MOUSE_MOVE && e->b < R.owner->mh) {
            int i = bar_item_at(R.owner, e->a);
            if (i >= 0 && i != R.bar) bar_open(R.owner, i);
            return 1;
        }
        if (e->type == OKE_MOUSE_BUTTON && e->d == 1) {
            if (e->b < R.owner->mh) { int i = bar_item_at(R.owner, e->a); if (i == R.bar || i < 0) R.done = 1; else bar_open(R.owner, i); return 1; }
            R.done = 1; return 1;
        }
    }
    if (e->type == OKE_MOUSE_BUTTON && e->d == 1) { R.done = 1; return 1; }
    if (e->type == OKE_KEY || e->type == OKE_CHAR || e->type == OKE_WHEEL) {
        if (npop) { if (e->type == OKE_KEY && e->b == 1) key_nav(&pops[npop - 1], e->a, 0); else if (e->type == OKE_CHAR) key_nav(&pops[npop - 1], e->a, 1); }
        return 1;
    }
    if (e->type == OKE_FOCUS) return 1;
    return 0;
}

extern int (*U_event_hook)(const struct okw_event *);
extern int U_pump(int ms);

static void bar_open(struct wnd *T, int idx) {
    pop_close_to(0);
    R.bar = idx;
    hot_bar = idx;
    U_menubar_paint(T);
    struct mitem *t = &T->menu->it[idx];
    if (!t->sub) { R.result = (int)t->id; R.done = 1; return; }
    int sx, sy; U_screen_origin(T, &sx, &sy);
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    pop_open(t->sub, sx + t->x - border, sy - T->mh + T->mh - border);       // under the bar item
    if (npop) {
        int first = -1;
        if (kbd_open) for (int i = 0; i < pops[0].m->n; i++) if (selectable(&pops[0].m->it[i])) { first = i; break; }
        set_hot(&pops[0], first);
    }
}

static int menu_run(struct wnd *owner, int bar_idx, HMENU popup, int sx, int sy, int returncmd) {
    if (R.owner) return 0;                                                  // a menu is already open
    memset(&R, 0, sizeof R);
    R.owner = owner; R.bar = bar_idx; R.returncmd = returncmd;
    U_event_hook = menu_hook;
    if (owner) { if (bar_idx >= 0) U_send(owner, WM_INITMENU, (WPARAM)owner->menu, 0); else U_send(owner, WM_INITMENU, (WPARAM)popup, 0); }
    if (bar_idx >= 0) bar_open(owner, bar_idx);
    else if (!pop_open(U_menu(popup), sx, sy)) R.done = 1;
    while (!R.done) { U_pump(30); if (npop == 0 && !R.done && bar_idx < 0) R.done = 1; }
    U_event_hook = 0;
    pop_close_to(0);
    int result = R.result;
    struct wnd *ow = R.owner;
    R.owner = 0; hot_bar = -1;
    if (ow && ow->used) {
        struct wnd *t = U_top(ow);
        if (bar_idx >= 0) U_menubar_paint(t);
        SC3(LX_ioctl, U_fd(), OKW_FOCUS, t->wid);
        if (result && !returncmd) PostMessageA(ow, WM_COMMAND, (WPARAM)(result & 0xFFFF), 0);
    }
    return result;
}

// Alt+letter (ch != 0) opens the bar menu with that mnemonic, F10 / a lone Alt (ch == 0) the first one: 1 = handled
int U_menubar_key(struct wnd *T, int ch) {
    if (!T->menu || R.owner || T->menu->n <= 0) return 0;
    int idx = -1;
    if (!ch) idx = 0;
    else {
        int c = ch; if (c >= 'A' && c <= 'Z') c += 32;
        for (int i = 0; i < T->menu->n && idx < 0; i++) {
            char plain[96]; int mn;
            split_text(T->menu->it[i].text, plain, sizeof plain, 0, 0, &mn);
            if (mn >= 0) { int m = plain[mn]; if (m >= 'A' && m <= 'Z') m += 32; if (m == c) idx = i; }
        }
    }
    if (idx < 0) return 0;
    kbd_open = 1;
    menu_run(T, idx, 0, 0, 0, 0);
    kbd_open = 0;
    return 1;
}

// the owner's events on its own menu bar (msg.c calls this for mouse events with y < mh)
int U_menubar_event(struct wnd *T, const struct okw_event *e) {
    if (!T->menu || R.owner) return 0;
    if (e->type == OKE_MOUSE_MOVE) {
        int i = bar_item_at(T, e->a);
        if (i != hot_bar) { hot_bar = i; U_menubar_paint(T); }
        return 1;
    }
    if (e->type == OKE_MOUSE_BUTTON && e->c == 1 && e->d == 1) {
        int i = bar_item_at(T, e->a);
        if (i >= 0) menu_run(T, i, 0, 0, 0, 0);
        return 1;
    }
    return 0;
}
void U_menubar_leave(struct wnd *T) { if (hot_bar >= 0) { hot_bar = -1; if (T->menu) U_menubar_paint(T); } }

EXPORT BOOL TrackPopupMenu(HMENU hm, UINT flags, int x, int y, int res, HWND hw, const RECT *rc) {
    (void)res; (void)rc;
    struct menu *m = U_menu(hm);
    struct wnd *w = U_wnd(hw);
    if (!m) return 0;
    if (flags & (TPM_CENTERALIGN | TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_VCENTERALIGN)) {
        struct pop tmp; memset(&tmp, 0, sizeof tmp); tmp.m = m; pop_measure(&tmp);
        if (flags & TPM_RIGHTALIGN) x -= tmp.w; else if (flags & TPM_CENTERALIGN) x -= tmp.w / 2;
        if (flags & TPM_BOTTOMALIGN) y -= tmp.h; else if (flags & TPM_VCENTERALIGN) y -= tmp.h / 2;
    }
    int r = menu_run(w, -1, hm, x, y, (flags & TPM_RETURNCMD) != 0);
    return (flags & TPM_RETURNCMD) ? r : r != 0;
}
// a list of choices (combo box drop-down): returns the id (1-based index) of the chosen item or 0
int U_list_popup(struct wnd *owner, struct menu *m, int sx, int sy, int minw, int sel) {
    g_minw = minw; g_sel = sel;
    int r = menu_run(owner, -1, m, sx, sy, 1);
    g_minw = 0; g_sel = -1;
    return r;
}

EXPORT BOOL TrackPopupMenuEx(HMENU hm, UINT flags, int x, int y, HWND hw, void *tpm) { (void)tpm; return TrackPopupMenu(hm, flags, x, y, 0, hw, 0); }

// ---- accelerators -------------------------------------------------------------------------------------------------------------------

struct accel { BYTE fVirt; WORD key, cmd; };
struct acctab { int magic, n; struct accel a[1]; };
#define ACC_MAGIC 0x41434354
#define FVIRTKEY 1
#define FSHIFT 4
#define FCONTROL 8
#define FALT 16

EXPORT HANDLE CreateAcceleratorTableA(const struct accel *a, int n) {
    struct acctab *t = umalloc(sizeof *t + (SIZE_T)n * sizeof(struct accel));
    if (!t) return 0;
    t->magic = ACC_MAGIC; t->n = n;
    for (int i = 0; i < n; i++) t->a[i] = a[i];
    return t;
}
EXPORT HANDLE CreateAcceleratorTableW(const struct accel *a, int n) { return CreateAcceleratorTableA(a, n); }
EXPORT BOOL DestroyAcceleratorTable(HANDLE h) { struct acctab *t = h; if (!t || t->magic != ACC_MAGIC) return FALSE; t->magic = 0; ufree(t); return TRUE; }
EXPORT HANDLE LoadAcceleratorsW(HINSTANCE h, LPCWSTR name) {
    HRSRC r = FindResourceExW(h, (LPCWSTR)(ULONG_PTR)9, name, 0);                   // RT_ACCELERATOR: {WORD flags (0x80 = last); WORD key; WORD cmd; WORD pad}
    if (!r) return 0;
    const WORD *p = LoadResource(h, r);
    int n = 0;
    while (n < 256) { n++; if (p[(n - 1) * 4] & 0x80) break; }
    struct accel a[256];
    for (int i = 0; i < n; i++) { a[i].fVirt = (BYTE)(p[i * 4] & 0x7F); a[i].key = p[i * 4 + 1]; a[i].cmd = p[i * 4 + 2]; }
    return CreateAcceleratorTableA(a, n);
}
EXPORT HANDLE LoadAcceleratorsA(HINSTANCE h, LPCSTR name) {
    WCHAR w[64];
    if ((ULONG_PTR)name < 0x10000) return LoadAcceleratorsW(h, (LPCWSTR)name);
    MultiByteToWideChar(65001, 0, name, -1, w, 64);
    return LoadAcceleratorsW(h, w);
}
static int translate_accel(HWND hw, HANDLE ha, MSG *m) {
    struct acctab *t = ha;
    if (!t || t->magic != ACC_MAGIC || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return 0;
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    for (int i = 0; i < t->n; i++) {
        struct accel *a = &t->a[i];
        if (a->key != (WORD)m->wParam && !(!(a->fVirt & FVIRTKEY) && a->key == (WORD)m->wParam)) continue;
        if (((a->fVirt & FSHIFT) != 0) != shift || ((a->fVirt & FCONTROL) != 0) != ctrl || ((a->fVirt & FALT) != 0) != alt) continue;
        U_send(U_wnd(hw), WM_COMMAND, (WPARAM)(a->cmd | (1 << 16)), 0);
        return 1;
    }
    return 0;
}
EXPORT int TranslateAcceleratorA(HWND hw, HANDLE ha, MSG *m) { return translate_accel(hw, ha, m); }
EXPORT int TranslateAcceleratorW(HWND hw, HANDLE ha, MSG *m) { return translate_accel(hw, ha, m); }
