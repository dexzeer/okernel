// xokai: an X11 server for the Linux subsystem. The top-level windows of the X clients become /dev/okwin windows (the
// desktop draws their frames, there is no X window manager); child windows are composited inside the top-level buffer.
// Freestanding: raw system calls only (lx/src/lxsys.h), no C library.
#ifndef XSRV_H
#define XSRV_H

#include "../src/lxsys.h"
#include "../../src/lx_okwin.h"

typedef unsigned short u16;
typedef short i16;
typedef int i32;
typedef signed char i8;

// ---- util.c -----------------------------------------------------------------------------------------------------------------------
void *memcpy(void *d, const void *s, u64 n);
void *memmove(void *d, const void *s, u64 n);
void *memset(void *d, int c, u64 n);
int memcmp(const void *a, const void *b, u64 n);
u64 strlen(const char *s);
int strcmp(const char *a, const char *b);
void *xmalloc(u64 n);
void *xcalloc(u64 n);
void xfree(void *p);
void *xrealloc(void *p, u64 n);
char *xstrndup(const char *s, u64 n);
void logf_(const char *fmt, ...);                                  // %d %u %x %s %c %lu %lx (to stderr)
#define LOG(...) logf_(__VA_ARGS__)
int glob_match(const char *pat, const char *s);                    // * and ?, case insensitive
u64 now_ms(void);
double dsin(double deg);
double dcos(double deg);
double dsqrt(double v);

static inline u32 g8(const u8 *b, int o) { return b[o]; }
static inline u32 g16(const u8 *b, int o) { return (u32)b[o] | ((u32)b[o + 1] << 8); }
static inline u32 g32(const u8 *b, int o) { return (u32)b[o] | ((u32)b[o + 1] << 8) | ((u32)b[o + 2] << 16) | ((u32)b[o + 3] << 24); }
static inline int gi16(const u8 *b, int o) { return (i16)g16(b, o); }
static inline void p8(u8 *b, int o, u32 v) { b[o] = (u8)v; }
static inline void p16(u8 *b, int o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static inline void p32(u8 *b, int o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); b[o + 2] = (u8)(v >> 16); b[o + 3] = (u8)(v >> 24); }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline u32 pad4(u32 n) { return (n + 3) & ~3u; }

// ---- regions ------------------------------------------------------------------------------------------------------------------------
typedef struct { int x, y, w, h; } Rect;
#define RGN_MAX 48
typedef struct { int n; Rect r[RGN_MAX]; } Region;
void rgn_set(Region *g, int x, int y, int w, int h);
void rgn_isect(Region *g, int x, int y, int w, int h);
void rgn_sub(Region *g, int x, int y, int w, int h);
void rgn_bbox(const Region *g, Rect *r);

// ---- protocol constants -------------------------------------------------------------------------------------------------------------
#define XRoot 0x10u
#define XVisual 0x21u
#define XColormap 0x22u

#define KeyPressMask 0x1
#define KeyReleaseMask 0x2
#define ButtonPressMask 0x4
#define ButtonReleaseMask 0x8
#define EnterWindowMask 0x10
#define LeaveWindowMask 0x20
#define PointerMotionMask 0x40
#define PointerMotionHintMask 0x80
#define Button1MotionMask 0x100
#define ButtonMotionMask 0x2000
#define ExposureMask 0x8000
#define VisibilityChangeMask 0x10000
#define StructureNotifyMask 0x20000
#define ResizeRedirectMask 0x40000
#define SubstructureNotifyMask 0x80000
#define SubstructureRedirectMask 0x100000
#define FocusChangeMask 0x200000
#define PropertyChangeMask 0x400000
#define ColormapChangeMask 0x800000

// event codes
enum { EV_KeyPress = 2, EV_KeyRelease, EV_ButtonPress, EV_ButtonRelease, EV_Motion, EV_Enter, EV_Leave, EV_FocusIn, EV_FocusOut,
       EV_KeymapNotify, EV_Expose, EV_GraphicsExpose, EV_NoExpose, EV_Visibility, EV_CreateNotify, EV_DestroyNotify,
       EV_UnmapNotify, EV_MapNotify, EV_MapRequest, EV_ReparentNotify, EV_ConfigureNotify, EV_ConfigureRequest,
       EV_GravityNotify, EV_ResizeRequest, EV_CirculateNotify, EV_CirculateRequest, EV_PropertyNotify, EV_SelectionClear,
       EV_SelectionRequest, EV_SelectionNotify, EV_ColormapNotify, EV_ClientMessage, EV_MappingNotify };
// error codes
enum { ER_Request = 1, ER_Value, ER_Window, ER_Pixmap, ER_Atom, ER_Cursor, ER_Font, ER_Match, ER_Drawable, ER_Access, ER_Alloc,
       ER_Colormap, ER_GC, ER_IDChoice, ER_Name, ER_Length, ER_Implementation };

// ---- resources ----------------------------------------------------------------------------------------------------------------------
enum { RES_WINDOW = 1, RES_PIXMAP, RES_GC, RES_FONT, RES_CURSOR, RES_COLORMAP };

struct client;
struct prop { u32 name, type; int format; u32 len; u8 *data; struct prop *next; };       // len = bytes
struct evsel { struct client *c; u32 mask; struct evsel *next; };

struct win {
    u32 id;
    struct win *parent, *first, *last, *prev, *next;       // children bottom -> top
    int x, y, w, h, border;                                // relative to the parent's inside origin
    int depth, inputonly;
    int mapped, override_redirect;
    int bg_mode; u32 bg_pixel, bg_pixmap;                  // 0 none, 1 pixel, 2 pixmap, 3 parent relative
    u32 border_pixel, dnp, cursor, colormap;
    int bit_gravity, win_gravity;
    struct evsel *sel;
    struct prop *props;
    struct client *owner;
    // top-level windows (children of the root)
    int wid;                                               // okwin window id, -1 = not created yet
    u32 *px; int bw, bh;                                   // the shared buffer
    int dx0, dy0, dx1, dy1;                                // dirty rectangle (empty when dx1 <= dx0)
    int rx0, ry0;                                          // client origin in screen coordinates (learned from pointer events)
    int wm_delete;
    u32 focus_child;                                       // not used by the desktop; kept for SetInputFocus on this top level
};

struct pix { u32 id; int w, h, depth; u32 *px; struct client *owner; };

struct gc {
    u32 id;
    int function; u32 plane_mask, fg, bg;
    int line_width, line_style, cap_style, join_style, fill_style, fill_rule, arc_mode;
    u32 tile, stipple; int ts_x, ts_y;
    u32 font;
    int subwindow_mode, graphics_exposures;
    int clip_x, clip_y; int nclip; Rect *clip;             // nclip < 0 = no clip rectangles
    int dash_offset, ndash; u8 dashes[16];
    struct client *owner;
};

struct fglyph { u8 *m; int w, h, asc; int done; };
struct font {
    u32 id; char name[100];
    int cw, asc, desc, size, bold;                         // cell width, ascent, descent in pixels; render size
    struct fglyph g[256];
    int refs;
};

#define MAXC 32
struct client {
    int fd, idx, state;                                    // state: 0 expects the setup request, 1 running
    u32 base;
    u16 seq;
    u8 *in; u32 inlen, incap;
    u8 *out; u32 outlen, outoff, outcap;
    int dead;
};

// ---- the display --------------------------------------------------------------------------------------------------------------------
extern int screen_w, screen_h;
extern struct win *root;
extern int okfd;
extern struct client *clients[MAXC];
extern struct client *cc;                                  // the client whose request is being handled
extern u32 srv_time(void);

void *res_get(u32 id, int kind);
void res_put(u32 id, int kind, void *p);
void res_del(u32 id);
struct win *win_of(u32 id);
int res_free_id(u32 id);

u32 atom_intern(const char *name, int len, int create);
const char *atom_name(u32 a);
#define A_PRIMARY 1
#define A_SECONDARY 2
#define A_ATOM 4
#define A_CARDINAL 6
#define A_STRING 31
#define A_WINDOW 33
#define A_WM_NAME 39
#define A_WM_CLASS 67
extern u32 a_wm_protocols, a_wm_delete, a_utf8, a_net_wm_name, a_clipboard, a_targets;

// output
u8 *out_reserve(struct client *c, u32 n);                  // appends n zeroed bytes
void reply_start(struct client *c, u8 *r, int data, u32 extra_bytes);   // fills the 8 byte reply header of a 32 byte reply
void send_error(struct client *c, int code, u32 val, int major, int minor);
void flush_clients(void);

// events (events.c)
void ev_to_client(struct client *c, const u8 *ev);         // stamps the sequence number
void ev_to_win(struct win *w, u32 mask, u8 *ev, int woff, int propagate);   // delivers to every client that selected `mask` on w (or on an ancestor)
u32 sel_union(struct win *w);
int win_has_mask(struct win *w, u32 mask);

// drawing
struct dstate {
    u32 *px; int sw, sh, depth;
    int ox, oy;                                            // drawable origin in surface coordinates
    Region clip;
    int bx0, by0, bx1, by1;                                // touched box (surface coordinates)
    struct gc *gc; struct win *top;
    int viewable;
};
int draw_open(u32 id, struct gc *gc, struct dstate *d);    // 0 = bad drawable, 1 = ok (d->clip may be empty)
void draw_close(struct dstate *d);
void draw_rect(struct dstate *d, int x, int y, int w, int h);
void draw_points(struct dstate *d, const u8 *pts, int n, int rel);
void draw_polyline(struct dstate *d, const u8 *pts, int n, int rel);
void draw_segments(struct dstate *d, const u8 *seg, int n);
void draw_rect_outline(struct dstate *d, int x, int y, int w, int h);
void draw_arc(struct dstate *d, int x, int y, int w, int h, int a1, int a2, int fill);
void draw_fillpoly(struct dstate *d, const u8 *pts, int n, int shape, int rel);
void draw_copy(struct dstate *dst, struct dstate *src, int sx, int sy, int w, int h, int dx, int dy);
void draw_copy_plane(struct dstate *dst, struct dstate *src, int sx, int sy, int w, int h, int dx, int dy, u32 plane);
void draw_text(struct dstate *d, struct font *f, int x, int y, const u8 *s, int n, int image, int wide);
void draw_put_image(struct dstate *d, int format, int depth, int w, int h, int dx, int dy, int left_pad, const u8 *data, u32 len);
u32 draw_get_image(struct dstate *d, int x, int y, int w, int h, u32 plane_mask, int format, u8 *out);   // returns bytes
void draw_bg(struct win *w, const Region *rg, int ox, int oy, u32 *px, int sw, int sh);   // paints the background of w into the region (surface coords)
void top_dirty(struct win *t, int x0, int y0, int x1, int y1);
void top_flush(struct win *t);

// fonts
struct font *font_open(u32 id, const char *name, int len);
int font_list(const char *pat, int len, char names[][100], int max);
void font_unref(struct font *f);
struct font *font_default(void);

// windows (win.c)
void win_init(void);
struct win *win_create(u32 id, struct win *parent, int x, int y, int w, int h, int border, int depth, int inputonly, struct client *owner);
void win_map(struct win *w);
void win_unmap(struct win *w);
void win_destroy(struct win *w, int notify);
void win_configure(struct win *w, u32 mask, const u32 *vals, u32 sibling);
void win_abs(struct win *w, int *ax, int *ay);             // inside origin in top-level buffer coordinates (the root: screen)
struct win *win_top(struct win *w);
int win_viewable(struct win *w);
void win_visible(struct win *w, Region *g, int clip_children);   // in top-level buffer coordinates
void win_expose_rect(struct win *top, int ax, int ay, int aw, int ah, int paint);
void win_expose_tree(struct win *w);
void win_change_attrs(struct win *w, u32 mask, const u32 *vals);
void win_set_title(struct win *w);
void win_prop_changed(struct win *w, u32 atom, int deleted);
void win_reparent(struct win *w, struct win *np, int x, int y);
struct win *win_at(struct win *top, int x, int y);         // deepest viewable window at a top-level buffer position
void win_client_gone(struct client *c);
void win_stack(struct win *w, int above_all);
void win_map_subs(struct win *w);
void win_unmap_subs(struct win *w);

// input (input.c)
void input_poll(void);
void input_init(void);
extern int grab_ptr_implicit, grab_ptr_owner_events; extern u32 grab_ptr_mask;
u32 keysym_for(int code, int idx);
extern struct win *focus_win;                              // SetInputFocus target (0 = follow the pointer, 1 = pointer root)
extern struct win *ptr_win;
extern struct win *grab_ptr_win, *grab_kb_win;
extern struct client *grab_ptr_client, *grab_kb_client;
extern int ptr_root_x, ptr_root_y; extern u32 ptr_state;
void input_win_gone(struct win *w);
void input_client_gone(struct client *c);
void xkeymap(int first, int count, u8 *out_syms_per, u32 *syms);   // keysyms for codes first.. (2 per code)
int input_query_pointer(void);

// selections
void sel_client_gone(struct client *c);
void sel_win_gone(struct win *w);

// requests
void req_dispatch(struct client *c, u8 *req, u32 len);
void req_setup(struct client *c, u8 *req, u32 len);
void req_client_gone(struct client *c);
int color_by_name(const char *s, int n, u32 *r, u32 *g, u32 *b);   // 16-bit components

// okwin helpers
int okw_ioctl(unsigned req, void *arg);
void okw_map_top(struct win *t);

#endif
