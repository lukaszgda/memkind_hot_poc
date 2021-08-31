#include "stdbool.h"
#include "stdlib.h"
#include "execinfo.h"
#include "threads.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ARRAYSZ(x) (sizeof(x)/sizeof((x)[0]))

static void *start[1024], *end[1024];
static int nm;
static void *stack0;
static void *pthread_start, *pthread_end; // hax!

// static void* csu_init

#define CUSTOM_BACKTRACE

void read_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char exec;
    int inode;

    nm=0;
    while (fscanf(f, "%p-%p %*c%*c%cp %*x %*x:%*x %d", &start[nm], &end[nm], &exec, &inode) == 4)
    {
        char *file = 0;
        size_t dummy = 0;
        if (getline(&file, &dummy, f)) ;
        if (strstr(file, "[stack]"))
            stack0 = end[nm];
        if (exec != 'x')
            continue;
        if (strstr(file, "libpthread"))
            pthread_start = start[nm], pthread_end = end[nm];
        if (nm++ >= ARRAYSZ(start))
            break;
    }
    fclose(f);
}
// end condition is kind of weak;
// apart from that, there are multiple entries for libpthread - currently, only the last one is taken into account, all others are ignored


static bool backtrace_unwinded(const void* addr, size_t idx) {
    return /* addr == __libc_csu_init || */ /* idx > 2 || */ (addr >= pthread_start && addr < pthread_end);
}

uint64_t bthash(uint64_t size)
{
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

#ifdef CUSTOM_BACKTRACE
    int i=0;
    for (void **sp = __builtin_frame_address(0); sp != stack0; sp++)
#else
    size_t SP_SIZE=100;
    void *sp[SP_SIZE];
    static thread_local bool backtrace_in_progress=false;
    if (backtrace_in_progress) {
        // bthash has called itself recursively
        // this can only be caused by backtrace -> malloc -> bthash
        // backtrace -> malloc call occurs only when dynamic library is loaded
        // in such case, we return hash "0" for dynamic library
        // TODO this is a workaround and should probably be handled
        // in a smarter way
        return 0; // valid hash - for a dynamic library
    }
    backtrace_in_progress=true;
    int bt_size = backtrace(sp, SP_SIZE);
    backtrace_in_progress=false;
    for (int i = 0; i<bt_size; i++)
#endif
    {
        volatile void *addr = sp[i];
        int s;
        for (s=0; s<nm; s++)
            if (start[s] > addr) // TODO explanation would be useful
                break;
        if (s-- && addr < end[s])
        {
            if (backtrace_unwinded((void*)addr, i))
                break;  // end hash calculation

            uint64_t k = (uintptr_t)addr;
            k *= M;
            k ^= k >> R;
            k *= M;
            h ^= k;
            h *= M;
        }
    }

#ifdef FINALIZE_HASH
    // Enable if we want "random" hash values.
    h ^= h >> R;
    h *= M;
    h ^= h >> R;
#endif

    return h;
}
