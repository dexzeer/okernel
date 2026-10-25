#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int total, ready;

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mu); total++; pthread_mutex_unlock(&mu); }
    pthread_mutex_lock(&mu); ready++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
    return (void*)(id * 10);
}

int main(void) {
    pthread_t th[4];
    for (long i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, (void*)i);
    pthread_mutex_lock(&mu);
    while (ready < 4) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    long sum = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); sum += (long)r; }
    printf("threads done: total=%d sum=%ld\n", total, sum);
    return total == 4000 && sum == 60 ? 0 : 1;
}
