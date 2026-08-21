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
// Tab strip 22 + toolbar 36 = 58 <= 96. Bands are tall enough for 1x text
// (16px glyphs): the old 18px tab strip half-covered the 32px body-font
// labels (toolbar painted over their lower half).
#define CHROME_TAB_H   22
#define CHROME_TOOL_H  36
#define CHROME_ROWS    3

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
#define TASKBAR_TOP       0x00263F6B
#define TASKBAR_BOT       0x00172841
#define TASK_BTN_INACT    0x003A5C95

#endif
