// Types and constants shared by our gdi32.dll and user32.dll
#ifndef GUI_H
#define GUI_H
#include "winbase.h"
#include "../../src/lx_okwin.h"

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
typedef struct { LONG bmType, bmWidth, bmHeight, bmWidthBytes; WORD bmPlanes, bmBitsPixel; LPVOID bmBits; } BITMAP;
typedef struct { LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY; CHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar; BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet; } TEXTMETRICA;
typedef struct { LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY; WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar; BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet; } TEXTMETRICW;
typedef struct { LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight; BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily; CHAR lfFaceName[32]; } LOGFONTA;
typedef struct { LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight; BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily; WCHAR lfFaceName[32]; } LOGFONTW;
typedef struct { UINT lbStyle; COLORREF lbColor; ULONG_PTR lbHatch; } LOGBRUSH;

typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct { UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; HICON hIconSm; } WNDCLASSEXA;
typedef struct { UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName; HICON hIconSm; } WNDCLASSEXW;
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; } WNDCLASSA;
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName; } WNDCLASSW;
typedef struct { LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent; int cy, cx, y, x; LONG style; LPCSTR lpszName, lpszClass; DWORD dwExStyle; } CREATESTRUCTA;

#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))
#define CLR_INVALID 0xFFFFFFFFu

#define SRCCOPY 0x00CC0020
#define SRCPAINT 0x00EE0086
#define SRCAND 0x008800C6
#define SRCINVERT 0x00660046
#define NOTSRCCOPY 0x00330008
#define PATCOPY 0x00F00021
#define PATINVERT 0x005A0049
#define DSTINVERT 0x00550009
#define BLACKNESS 0x00000042
#define WHITENESS 0x00FF0062

#define OBJ_PEN 1
#define OBJ_BRUSH 2
#define OBJ_DC 3
#define OBJ_FONT 6
#define OBJ_BITMAP 7
#define OBJ_REGION 8

#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_NULL 5
#define BS_SOLID 0
#define BS_NULL 1
#define BS_HOLLOW 1
#define BS_HATCHED 2
#define TRANSPARENT 1
#define OPAQUE 2
#define DIB_RGB_COLORS 0
#define BI_RGB 0
#define BI_BITFIELDS 3

#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define SYSTEM_FONT 13
#define DEFAULT_GUI_FONT 17
#define SYSTEM_FIXED_FONT 16
#define ANSI_FIXED_FONT 11
#define ANSI_VAR_FONT 12
#define DC_BRUSH 18
#define DC_PEN 19

#define DT_LEFT 0
#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define DT_END_ELLIPSIS 0x8000

// messages
#define WM_NULL 0
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_MOVE 3
#define WM_SIZE 5
#define WM_ACTIVATE 6
#define WM_SETFOCUS 7
#define WM_KILLFOCUS 8
#define WM_ENABLE 10
#define WM_PAINT 15
#define WM_CLOSE 16
#define WM_QUIT 18
#define WM_ERASEBKGND 20
#define WM_SHOWWINDOW 24
#define WM_ACTIVATEAPP 28
#define WM_SETCURSOR 32
#define WM_MOUSEACTIVATE 33
#define WM_GETMINMAXINFO 36
#define WM_NCCREATE 129
#define WM_NCDESTROY 130
#define WM_NCCALCSIZE 131
#define WM_NCHITTEST 132
#define WM_NCPAINT 133
#define WM_GETTEXT 13
#define WM_GETTEXTLENGTH 14
#define WM_SETTEXT 12
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_CHAR 0x102
#define WM_SYSKEYDOWN 0x104
#define WM_SYSKEYUP 0x105
#define WM_COMMAND 0x111
#define WM_TIMER 0x113
#define WM_HSCROLL 0x114
#define WM_VSCROLL 0x115
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_LBUTTONDBLCLK 0x203
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_MBUTTONDOWN 0x207
#define WM_MBUTTONUP 0x208
#define WM_MOUSEWHEEL 0x20A
#define WM_USER 0x400
#define WM_APP 0x8000

#define CS_VREDRAW 1
#define CS_HREDRAW 2
#define CS_DBLCLKS 8
#define WS_OVERLAPPED 0
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000
#define WS_VISIBLE 0x10000000
#define WS_CAPTION 0x00C00000
#define WS_BORDER 0x00800000
#define WS_SYSMENU 0x00080000
#define WS_THICKFRAME 0x00040000
#define WS_MINIMIZEBOX 0x00020000
#define WS_MAXIMIZEBOX 0x00010000
#define WS_OVERLAPPEDWINDOW (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 8
#define BS_PUSHBUTTON 0
#define BS_DEFPUSHBUTTON 1
#define ES_AUTOHSCROLL 0x80
#define WS_EX_CLIENTEDGE 0x200
#define WS_TABSTOP 0x00010000
#define CW_USEDEFAULT ((int)0x80000000)
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define PM_NOREMOVE 0
#define PM_REMOVE 1
#define MB_OK 0
#define MB_OKCANCEL 1
#define MB_YESNO 4
#define MB_ICONERROR 0x10
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define GWLP_WNDPROC (-4)
#define GWLP_ID (-12)
#define COLOR_WINDOW 5
#define COLOR_BTNFACE 15
#define COLOR_WINDOWTEXT 8
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define VK_BACK 8
#define VK_TAB 9
#define VK_RETURN 13
#define VK_SHIFT 16
#define VK_CONTROL 17
#define VK_MENU 18
#define VK_ESCAPE 27
#define VK_SPACE 32
#define VK_LEFT 37
#define VK_UP 38
#define VK_RIGHT 39
#define VK_DOWN 40
#define VK_DELETE 46
#define MK_LBUTTON 1
#define MK_RBUTTON 2
#define MK_SHIFT 4
#define MK_CONTROL 8
#define MK_MBUTTON 16

// private cooperation between user32 and gdi32 (exported with a double underscore)
HDC __gdi_make_dc(unsigned *px, int w, int h, int stride);
void __gdi_dc_set_surface(HDC dc, unsigned *px, int w, int h, int stride);
void __gdi_free_dc(HDC dc);
BOOL __gdi_rect_fill_color(HDC dc, int x0, int y0, int x1, int y1, COLORREF c);
int __gdi_FillRect(HDC dc, const RECT *r, HBRUSH b);
int __gdi_FrameRect(HDC dc, const RECT *r, HBRUSH b);
void __gdi_dc_setup(HDC dc, unsigned *px, int w, int h, int stride, int orgx, int orgy, const RECT *clip);
void __gdi_reset_dc(HDC dc);
int __gdi_is_dc(HDC dc);

#endif
