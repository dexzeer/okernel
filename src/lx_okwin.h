#ifndef LX_OKWIN_H
#define LX_OKWIN_H

// /dev/okwin: the native window interface of the Linux subsystem. A program opens it once (a connection), creates windows
// (each has a shared 32-bit BGRX pixel buffer it maps into its address space), draws, tells the kernel which part changed
// (present) and reads input events. Win32 (user32/gdi32) and an X11 server are built on it in user space.
//
// This header is shared with user space (copied into lx/win/okwin.h): plain types only.

#define OKW_CREATE   0x4F01
#define OKW_DESTROY  0x4F02
#define OKW_MAP      0x4F03
#define OKW_PRESENT  0x4F04
#define OKW_SETTITLE 0x4F05
#define OKW_RESIZE   0x4F06
#define OKW_MOVE     0x4F07
#define OKW_SHOW     0x4F08
#define OKW_INFO     0x4F09
#define OKW_FOCUS    0x4F0A
#define OKW_TEXT     0x4F0B     /* render text with the system font into a window buffer */
#define OKW_CLIPBOARD 0x4F0C
#define OKW_QUERY    0x4F0D     /* window position / pointer state */

struct okw_create  { int x, y, w, h; unsigned flags; char title[64]; int out_wid; int out_stride; };
struct okw_map     { int wid; int pad; unsigned long long out_addr; unsigned out_size; unsigned pad2; };
struct okw_present { int wid; int x, y, w, h; };
struct okw_title   { int wid; char title[64]; };
struct okw_resize  { int wid; int w, h; int pad; unsigned long long out_addr; };
struct okw_move    { int wid; int x, y; };
struct okw_show    { int wid; int show; };
struct okw_info    { int screen_w, screen_h; int title_h, border; };
struct okw_text    { unsigned long long str; int len, size, flags, family; unsigned long long mask; int cap_w, cap_h, out_w, out_asc, out_desc, pad; };   /* flags: 1 bold 2 italic 4 measure only; family 0 sans 1 serif 2 mono; mask = cap_w x cap_h coverage bytes */

struct okw_query   { int wid; int x, y, w, h; int mx, my, mb, focused; };   /* in: wid (or -1);  out: outer position and client size of the window, pointer, focus */

#define OKW_F_NOFRAME 1          /* undecorated window (menus, tooltips) */
#define OKW_F_HIDDEN 2           /* created hidden: OKW_SHOW 1 shows it (with the opening animation) */

/* input events, read() from the connection */
#define OKE_MOUSE_MOVE 1         /* a,b = x,y in the window            */
#define OKE_MOUSE_BUTTON 2       /* a,b = x,y   c = 1 left 2 right 3 middle   d = 1 down 0 up */
#define OKE_WHEEL 3              /* a,b = x,y   c = notches (positive = away from the user)   */
#define OKE_KEY 4                /* a = Linux key code  b = 1 down 0 up  d = modifiers (1 shift 2 ctrl 4 alt) */
#define OKE_CHAR 5               /* a = character typed (ASCII / UTF-32), d = modifiers      */
#define OKE_FOCUS 6              /* a = 1 gained 0 lost                */
#define OKE_CLOSE 7              /* the user asked to close the window */
#define OKE_RESIZE 8             /* a,b = new client size              */
struct okw_event { unsigned type; int wid; int a, b, c, d; };

#endif
