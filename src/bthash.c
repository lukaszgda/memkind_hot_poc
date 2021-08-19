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

#include "assert.h"
// static volatile size_t iterated_addresses=0;
// static volatile size_t used_addresses=0;

#include "stdbool.h"

static bool top_reached(const void* addr, size_t idx) {
    return /* addr == __libc_csu_init || */ /* idx > 2 || */ (addr >= pthread_start && addr < pthread_end);
}

#include "stdlib.h"
#include "execinfo.h"

uint64_t bthash(uint64_t size)
{
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

//     static volatile void *addresses[50];
//     used_addresses = 0;

//     for (void **sp = __builtin_frame_address(0); sp != stack0; sp++)
    size_t SP_SIZE=100;
    void *sp[SP_SIZE];
    int bt_size = backtrace(sp, SP_SIZE);
    for (int i = 0; i<bt_size; i++)
    {
        volatile void *addr = sp[i];
        int s;
        for (s=0; s<nm; s++)
            if (start[s] > addr) // TODO explanation would be useful
                break;
        if (s-- && addr < end[s])
        {
            if (top_reached((void*)addr, i)) // pthread reached
                break;  // end hash calculation

//             addresses[i++] = addr;
//             ++used_addresses;
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
//     iterated_addresses=i;
//     if (iterated_addresses == 99999)
//         for (int j=0; j<i; ++j)
//             printf("%p\n", addresses[j]);
//     used_addresses;

    return h;
}
