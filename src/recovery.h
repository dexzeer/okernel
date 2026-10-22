#ifndef RECOVERY_H
#define RECOVERY_H

// The recovery desktop's commands (recovery.c), run by the desktop shell when the kernel was booted with `recovery`.
int recovery_crash_window(void);                                  // the last boot's crash, a closable window (-1: none)
void recovery_help(int win);
int recovery_command(int win, const char* cmd, const char* args); // 1 when cmd was a recovery command

#endif
