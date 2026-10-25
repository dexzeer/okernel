#ifndef TASKBAR_H_MOD
#define TASKBAR_H_MOD

// Desktop taskbar (taskbar.c). TASKBAR_H itself lives in window.h.
#define TB_HIT_NONE  (-1)

// Window id under (mx, my), or TB_HIT_NONE.
int  taskbar_hit(int mx, int my);
// Blit the (cached) bar, honoring the current clip — for damage repair.
void taskbar_paint(void);
// Per frame: re-render + blit only if something changed; returns 1 if it did.
int  taskbar_update(unsigned fps);
void taskbar_invalidate(void);
// Screen rectangle {x, y, w, h} of a window's taskbar button; 0 if it has none.
int  taskbar_button_rect(int id, int r[4]);
// Draw the 20 px app icon of window `id` (the same icon as on its taskbar button).
struct wsurf;
void taskbar_draw_window_icon(struct wsurf* s, int id, int x, int y);

#endif
