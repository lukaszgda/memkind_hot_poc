// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021 Intel Corporation. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ARRAYSZ(x) (sizeof(x)/sizeof((x)[0]))

static void *start[1024], *end[1024];
static int nm;
static void *pthread_start, *pthread_end; // hax!

extern void _start(void);

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
        if (exec != 'x')
            continue;
        if (file && strstr(file, "libpthread"))
            pthread_start = start[nm], pthread_end = end[nm];
        free(file);
        if (nm++ >= ARRAYSZ(start))
            break;
    }
    fclose(f);
}

uint64_t bthash(uint64_t size)
{
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

    for (void **sp = __builtin_frame_address(0);; sp++)
    {
        void *addr = *sp;
        int s;
        for (s=0; s<nm; s++)
            if (start[s] > addr)
                break;
        if (s-- && addr < end[s])
        {
            if (addr == _start || (addr >= pthread_start && addr < pthread_end))
                break;

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
