#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

// Ticket-free spinlock via xchg (SMP-safe on real hardware; on our single CPU
// it mainly guards IRQ-vs-thread races). IRQ-safe variants disable interrupts
// around the critical section so an IRQ handler can take the same lock.
typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t *l) { l->locked = 0; }

static inline void spin_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        __asm__ volatile("pause");
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __sync_lock_release(&l->locked);
}

// IRQ-safe: returns EFLAGS so the caller can restore (sti only if on entry).
static inline uint32_t spin_lock_irq(spinlock_t *l) {
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
    spin_lock(l);
    return eflags;
}

static inline void spin_unlock_irq(spinlock_t *l, uint32_t eflags) {
    spin_unlock(l);
    if (eflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

// One-shot mutex (sleep-free: spins). Built on the spinlock; the owner field
// is advisory (no thread ids yet) — used for documentation + nesting checks.
typedef struct {
    spinlock_t lock;
    int holder;   // pid of holder, -1 = free
    int depth;    // nesting depth for the same holder
} mutex_t;

#define MUTEX_INIT { SPINLOCK_INIT, -1, 0 }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m, int pid);
void mutex_unlock(mutex_t *m, int pid);

#endif
