#include "spinlock.h"
#include <stdint.h>

void mutex_init(mutex_t *m) {
    spin_init(&m->lock);
    m->holder = -1;
    m->depth = 0;
}

void mutex_lock(mutex_t *m, int pid) {
    // Fast path: same holder re-enters (recursion without deadlock).
    // The check-then-lock is racy on SMP, but single-CPU + IRQ-guarded
    // callers make it exact here; documented, not relied upon for safety.
    if (m->holder == pid) {
        m->depth++;
        return;
    }
    spin_lock(&m->lock);
    m->holder = pid;
    m->depth = 1;
}

void mutex_unlock(mutex_t *m, int pid) {
    if (m->holder != pid) return; // not ours — ignore (fail safe)
    if (--m->depth == 0) {
        m->holder = -1;
        spin_unlock(&m->lock);
    }
}
