// A real Xlib client (libX11 from the host, a test input only, linked by file name: no X headers needed) against xokai.
#include <stdio.h>
#include <string.h>

typedef struct _XDisplay Display;
typedef unsigned long Window, GC, Atom, Font, Time, KeySym;
typedef struct { short x, y; unsigned short width, height; } XRect;
typedef struct { short x1, y1, x2, y2; } XSeg;
typedef struct { short x, y; } XPt;
typedef union {
    int type;
    struct { int type; unsigned long serial; int send_event; Display *display; Window window; int x, y, width, height, count; } xexpose;
    struct { int type; unsigned long serial; int send_event; Display *display; Window window, root, subwindow; Time time; int x, y, x_root, y_root; unsigned state, button; int same_screen; } xbutton;
    struct { int type; unsigned long serial; int send_event; Display *display; Window window, root, subwindow; Time time; int x, y, x_root, y_root; unsigned state, keycode; int same_screen; } xkey;
    struct { int type; unsigned long serial; int send_event; Display *display; Window window; Atom message_type; int format; long data[5]; } xclient;
    long pad[24];
} XEvent;
typedef struct { short lbearing, rbearing, width, ascent, descent; unsigned short attributes; } XCharStruct;
typedef struct { void *ext; Font fid; unsigned direction, min_char_or_byte2, max_char_or_byte2, min_byte1, max_byte1; int all_chars_exist; unsigned default_char; int n_properties; void *properties;
                 XCharStruct min_bounds, max_bounds; XCharStruct *per_char; int ascent, descent; } XFontStruct;

Display *XOpenDisplay(const char *);
int XDefaultScreen(Display *);
Window XRootWindow(Display *, int);
Window XCreateSimpleWindow(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
int XStoreName(Display *, Window, const char *);
int XSelectInput(Display *, Window, long);
int XMapWindow(Display *, Window);
int XNextEvent(Display *, XEvent *);
int XPending(Display *);
GC XCreateGC(Display *, Window, unsigned long, void *);
int XSetForeground(Display *, GC, unsigned long);
int XFillRectangle(Display *, Window, GC, int, int, unsigned, unsigned);
int XDrawRectangle(Display *, Window, GC, int, int, unsigned, unsigned);
int XDrawLine(Display *, Window, GC, int, int, int, int);
int XDrawString(Display *, Window, GC, int, int, const char *, int);
int XFlush(Display *);
int XSync(Display *, int);
Atom XInternAtom(Display *, const char *, int);
int XSetWMProtocols(Display *, Window, Atom *, int);
XFontStruct *XLoadQueryFont(Display *, const char *);
int XSetFont(Display *, GC, Font);
int XTextWidth(XFontStruct *, const char *, int);
KeySym XLookupKeysym(XEvent *, int);
char *XKeysymToString(KeySym);
int XLookupString(XEvent *, char *, int, KeySym *, void *);
int XCloseDisplay(Display *);
int XFreeGC(Display *, GC);
Window XCreateWindow(Display *, Window, int, int, unsigned, unsigned, unsigned, int, unsigned, void *, unsigned long, void *);
unsigned long XBlackPixel(Display *, int);
unsigned long XWhitePixel(Display *, int);
void XFree(void *);
int XFillArc(Display *, Window, GC, int, int, unsigned, unsigned, int, int);
int XQueryPointer(Display *, Window, Window *, Window *, int *, int *, int *, int *, unsigned *);
void *XGetImage(Display *, Window, int, int, unsigned, unsigned, unsigned long, int);
int XChangeProperty(Display *, Window, Atom, Atom, int, int, const unsigned char *, int);
int XGetWindowProperty(Display *, Window, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **);

#define ExposureMask 0x8000
#define KeyPressMask 1
#define ButtonPressMask 4
#define StructureNotifyMask 0x20000

int main(void) {
    Display *d = XOpenDisplay(":0");
    if (!d) { puts("xlib: cannot open display"); return 1; }
    int s = XDefaultScreen(d);
    Window root = XRootWindow(d, s);
    Window w = XCreateSimpleWindow(d, root, 40, 40, 320, 200, 1, 0, 0xF0F0F0);
    XStoreName(d, w, "xlib test");
    XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(d, w, &del, 1);
    GC gc = XCreateGC(d, w, 0, 0);
    XFontStruct *f = XLoadQueryFont(d, "fixed");
    if (f) { XSetFont(d, gc, f->fid); printf("xlib: font ascent %d descent %d width %d\n", f->ascent, f->descent, XTextWidth(f, "hello", 5)); }
    else puts("xlib: no font");
    XMapWindow(d, w);
    printf("xlib: black %lx white %lx\n", XBlackPixel(d, s), XWhitePixel(d, s));
    fflush(stdout);
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == 12) {                                  // Expose
            if (e.xexpose.count) continue;
            XSetForeground(d, gc, 0xC00000); XFillRectangle(d, w, gc, 20, 20, 90, 50);
            XSetForeground(d, gc, 0x0000C0); XFillRectangle(d, w, gc, 130, 20, 90, 50);
            XSetForeground(d, gc, 0x000000); XDrawRectangle(d, w, gc, 10, 10, 220, 70);
            XDrawLine(d, w, gc, 10, 100, 300, 150);
            XSetForeground(d, gc, 0x008000); XFillArc(d, w, gc, 230, 20, 60, 60, 0, 360 * 64);
            XSetForeground(d, gc, 0x000000); XDrawString(d, w, gc, 20, 130, "Hello from Xlib", 15);
            XSync(d, 0);
            Window rr, cr; int rx, ry, wx, wy; unsigned mask;
            int ok = XQueryPointer(d, w, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
            printf("xlib: drawn pointer=%d\n", ok);
            fflush(stdout);
        } else if (e.type == 4) {
            printf("xlib: button %u at %d,%d\n", e.xbutton.button, e.xbutton.x, e.xbutton.y);
            fflush(stdout);
        } else if (e.type == 2) {
            char buf[16]; KeySym ks = 0;
            int n = XLookupString(&e, buf, 15, &ks, 0);
            buf[n > 0 ? n : 0] = 0;
            printf("xlib: key keycode=%u keysym=%lx str='%s'\n", e.xkey.keycode, ks, buf);
            fflush(stdout);
            if (ks == 0xff1b) break;
        } else if (e.type == 33) {
            if ((Atom)e.xclient.data[0] == del) { puts("xlib: close"); break; }
        }
    }
    XCloseDisplay(d);
    puts("xlib: bye");
    return 0;
}
