#ifndef GAMEWIN_H
#define GAMEWIN_H

// The game window: runs a dmccl level (src/game, games/*.dmccl) in a desktop window.
//   game            list the levels
//   game 2          start the built-in level 2
//   game FILE       run a level script from the file system (write one with the editor)
// Keys: arrows / WASD drive and lean, space ejects, R restarts, N next level, Esc closes. Held keys come from the keyboard driver.
// Returns 0 when the window opened, otherwise a message for the terminal.
const char* gamewin_open(const char* what);
int  gamewin_win(void);                 // window id, or -1 when closed
void gamewin_close(void);
void gamewin_key(char c);               // a typed key (R, N, Esc ...)
void gamewin_poll(void);                // every main-loop pass: read held keys, advance the world, draw
int  gamewin_level_count(void);
const char* gamewin_level_name(int i);

#endif
