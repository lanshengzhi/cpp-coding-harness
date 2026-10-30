#define _GNU_SOURCE
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Per-process CLOCK_REALTIME jumps; CLOCK_MONOTONIC stays on the real clock. */
int clock_gettime(clockid_t clock_id, struct timespec* value) {
    static _Thread_local unsigned int calls;
    int result = syscall(SYS_clock_gettime, clock_id, value);
    if (result == 0 && clock_id == CLOCK_REALTIME && (calls++ & 1)) {
        value->tv_sec += 86400;
    }
    return result;
}
