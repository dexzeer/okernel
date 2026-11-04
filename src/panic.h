#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

// The panic screen (panic.c): shows what went wrong, saves a crash record to the installed disk's log partition, and
// reboots into the recovery system. Desktop kernel only.
struct trapframe;
void panic_exception(int vec, struct trapframe* frame) __attribute__((noreturn));   // from isr_handler (idt.c)
void kpanic(const char* fmt, ...) __attribute__((noreturn));               // a fatal error found by kernel code

#endif
