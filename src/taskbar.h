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

#endif
