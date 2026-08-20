#ifndef SHELL_H
#define SHELL_H

// Initialize the shell
void shell_init(void);

// Process a line of input
void shell_execute(const char* input);

// Print the shell prompt
void shell_prompt(void);

#endif
