#ifndef WINFX_H
#define WINFX_H

// Window chrome effects: rounded corners and the open / minimize / restore animations (winfx.c).

#define WIN_RADIUS 12      // corner radius of a window (0 while maximized)

// Round the corners of window `id` inside the screen rectangle [x0,x1) x [y0,y1) that was just (re)painted square: the
// pixels outside the arc become what is behind the window, the edge is anti-aliased, the border follows the curve.
// Idempotent: calling it again on already rounded pixels changes nothing.
void winfx_round_corners(int id, int x0, int y0, int x1, int y1);
int  winfx_radius(int id);

// The animations. open: a new window grows and fades in. minimize: the window shrinks into its taskbar button.
// restore: the reverse. During an animation the window is not painted by the compositor (winfx_hides); the animation
// draws it. All three fall back to "no animation" when memory is short.
void winfx_open(int id);                 // call right after window_create (the first winfx_step starts it)
int  winfx_minimize(int id);             // 1 = animating (the caller sets minimized as usual)
int  winfx_restore(int id);              // 1 = animating (the caller has cleared minimized)
void winfx_cancel(int id);               // the window is destroyed
int  winfx_hides(int id);                // the compositor must not paint this window now
int  winfx_active(void);                 // an animation is running (keeps the main loop awake)
void winfx_step(void);                   // once per main-loop pass, before the windows are drawn

#endif
