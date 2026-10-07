#ifndef THEME_H
#define THEME_H

#include <stdint.h>

// Firefox-ish blue theme. Colors are 0x00RRGGBB (matches graphics.c pixel format).
#define CHROME_TAB_TOP    0x005A7FC0
#define CHROME_TAB_BOT    0x003A5C95
#define CHROME_TOOL_TOP   0x006E92C8 // lighter than the tab strip's bottom, so the two bands read as distinct
#define CHROME_TOOL_BOT   0x00263F6B
#define CHROME_TAB_ACTIVE 0x00EAF1FB // light active tab pops against the blue strip
#define CHROME_TAB_TEXT   0x00000000
#define CHROME_TAB_INACT  0x00C2CEDF
#define CHROME_BTN_FG     0x00FFFFFF
#define ADDR_BG           0x00FFFFFF
#define ADDR_BORDER       0x006C7682
#define ADDR_TEXT         0x00000000
#define LOCK_OK           0x0040C060
#define ACCENT_BLUE       0x000050C0
#define CHROME_TEXT       0x00FFFFFF

// Chrome layout in pixels. Tab strip + toolbar must fit within
// CHROME_ROWS * CHAR_H reserved content-buffer rows (3 * 32 = 96px).
// Tab strip 38 + toolbar 58 = 96px EXACTLY, so the page content begins right
// at the toolbar bottom (no blank gap and no overlap with the chrome band).
// Chrome rows must be tall enough for the 32px body-font glyphs (FONT_H).
// The tab strip and toolbar each reserve room for one 32px line plus padding
// so labels like "okai:home" and tab titles show in full instead of being
// clipped to their upper half. CHROME_ROWS reserves that many 32px content
// buffer rows (CHROME_TAB_H + CHROME_TOOL_H <= CHROME_ROWS * FONT_H).
#define CHROME_TAB_H   44
#define CHROME_TOOL_H  52
#define CHROME_ROWS    3

// --- okai browser chrome: Firefox "Proton" light palette (okai_ui.c) -------
#define FX_FRAME          0x00F0F0F4 // tab strip
#define FX_FRAME_U        0x00EBEBEF // tab strip, window unfocused
#define FX_TAB_SEL        0x00FFFFFF // selected tab (floats over the strip)
#define FX_TAB_HOVER      0x00E0E0E6 // hovered background tab / buttons on the strip
#define FX_TOOLBAR        0x00F9F9FB // nav bar
#define FX_TOOLBAR_SEP    0x00CFCFD8 // 1px line between nav bar and page
#define FX_BTN_HOVER      0x00E0E0E6 // toolbar button hover
#define FX_URLBAR         0x00F0F0F4 // address field
#define FX_URLBAR_HOVER   0x00E8E8ED
#define FX_FOCUS          0x000061E0 // focus ring / accent
#define FX_TEXT           0x0015141A
#define FX_TEXT_2         0x005B5B66 // secondary (URL path, placeholders)
#define FX_ICON           0x002B2A33
#define FX_ICON_OFF       0x00ABABB4 // disabled back/forward
#define FX_CLOSE_HOVER    0x00E81123 // caption close hover (white glyph)
#define FX_SECURE         0x002B2A33 // lock (Firefox keeps it neutral)
#define FX_INSECURE       0x00E22850
#define FX_FRAME_BORDER   0x00A0A0AB // window frame around the browser
#define FX_FRAME_BORDER_U 0x00CFCFD8

// Nav toolbar buttons: rounded dark chips on the toolbar gradient
#define NAVBTN_BG     0x00284A78
#define NAVBTN_HI     0x004A6C9F // 1px top highlight
#define NAVBTN_R      6          // corner radius

// --- Unified OS chrome (window title bars, borders, taskbar) ---------------
// Same visual language as the browser toolbar so the whole desktop reads as
// one system instead of Win95-with-a-modern-browser.
#define TITLE_GRAD_U_TOP  0x00606876 // unfocused: desaturated gray-blue
#define TITLE_GRAD_U_BOT  0x00424A58
#define TITLE_TEXT_F      0x00FFFFFF // focused title text
#define TITLE_TEXT_U      0x00C6CDD8 // unfocused title text
#define TITLE_DIVIDER     0x001A2E4D // crisp bottom edge under the title bar
#define BORDER_ACTIVE     0x003A5C95
#define BORDER_INACTIVE   0x005A6270
#define TBTN_CLOSE_BG     0x00C04040 // muted red close
#define TBTN_NEUTRAL_BG   0x00444B57 // slate minimize
#define TBTN_FG           0x00FFFFFF
// Title-bar windows (terminal, editor, sysinfo): GNOME/libadwaita dark header bar.
#define HDR_BG            0x002E2E33
#define HDR_BG_U          0x00242428
#define HDR_DIVIDER       0x0018181B
#define HDR_BORDER        0x0046464E
#define HDR_BORDER_U      0x0030303A
#define HDR_BTN           0x0046464E // round button disc
#define HDR_BTN_U         0x00343439
#define HDR_TEXT          0x00F2F2F5
#define HDR_TEXT_U        0x008E8E98

#endif
