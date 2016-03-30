#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include "spin.h"

static int __cpus = 1;


void spin_init()
{
    __cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (__cpus <= 0) {
        __cpus = 1;
    }
}


void spin_lock(spin_t *lock)
{
    int i, n;

    for ( ;; ) {
        if (*lock == 0 &&
            __sync_bool_compare_and_swap(lock, 0, 1)) {
            return;
        }

        if (__cpus > 1) {
            for (n = 1; n < 129; n << 1) {
                for (i = 0; i < n; i++) {
                    __asm__("pause");
                }

                if (*lock == 0 &&
                    __sync_bool_compare_and_swap(lock, 0, 1)) {
                    return;
                }
            }
        }

        sched_yield();
    }
}


void spin_unlock(spin_t *lock)
{
    __sync_bool_compare_and_swap(lock, 1, 0);
}

