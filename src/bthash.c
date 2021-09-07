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
static void *stack_start, *stack_end;
static void *pthread_start, *pthread_end; // hax!

// static void* csu_init

#define CUSTOM_BACKTRACE

// TODO test this function
static void preprocess_maps(void) {
    // the maps are already sorted; potential for optimisation: merge regions
    int inew=0;
    for (int i=0; i<nm-1; ++i) {
        // check if ith region can be merged with i+1th region
        bool can_be_merged = end[inew]==start[i+1];
        if (can_be_merged) {
            end[inew]=end[i+1];
        } else {
            ++inew;
        }
    }
    if (nm>0) {
        nm=inew+1;
    }
}

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
        if (strstr(file, "[stack]")) {
            stack_start = start[nm];
            stack_end = start[nm];
            stack0 = end[nm];
        }
        if (exec != 'x')
            continue;
        if (strstr(file, "libpthread"))
            pthread_start = start[nm], pthread_end = end[nm];
        if (nm++ >= ARRAYSZ(start))
            break;
    }
    fclose(f);
    preprocess_maps();
}
// end condition is kind of weak;
// apart from that, there are multiple entries for libpthread - currently, only the last one is taken into account, all others are ignored


static bool backtrace_unwinded(const void* addr, size_t idx) {
    return /* addr == __libc_csu_init || */ /* idx > 2 || */ (addr >= pthread_start && addr < pthread_end);
}

#ifdef CUSTOM_BACKTRACE

/// Find highest start idx that lies before @p ptr
/// @pre ptr >= start[0] && ptr < end[nm]
static int binsearch_start(const void *ptr) {
    // bstart always "lies before ptr", bend: sometimes
    int bstart=0, bend=nm-1;
    while (bstart != bend) {
        int mid = (bstart+bend)/2;
        if (ptr >= start[mid]) {
            if (bstart == mid) {
                // check if bend is still ok
                if (ptr >= start[bend])
                    bstart=bend;
                break;
            }
            bstart = mid;
        } else {
            bend = mid;
        }
    }
    return bstart;
}

/// Find lowest end idx that lies after @p ptr
/// @pre ptr >= start[0] && ptr < end[nm]
static int binsearch_end(const void *ptr) {
    // bend always "lies after ptr", bstart: never
    int bstart=0, bend=nm-1;
    while (bstart != bend) {
        int mid = (bstart+bend)/2;
        if (ptr < end[mid]) {
            bend = mid;
        } else {
            if (bstart == mid) {
                // bstart is not ok, bend is ok; no additional action required
                break;
            }
            bstart = mid;
        }
    }
    return bend;
}

/// @pre  addr < start[0] || addr >= end[nm-1]
static int find_region_idx(void *addr)
{
    if (addr < start[0] || addr >= end[nm-1])
        return -1;
    int bstart = binsearch_start(addr);
    int bend = binsearch_end(addr);
    if (bstart == bend) {
        return bstart;
    }
    return -1;
}

static bool is_on_stack(const void* ptr) {
    return ptr >= stack_start && ptr < stack_end;
}

#include "assert.h"

uint64_t bthash(uint64_t size)
{
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

    // can we directly obtain stack pointer?
//     void *stock_ptr = __builtin_frame_address(0);
//     for (void **sp = __builtin_frame_address(1); sp != stack0; sp++)
    for (void **sp = __builtin_frame_address(0); sp != stack0; sp++)
    {
//         assert(is_on_stack(sp));
        void *addr=*sp;
#if 1
        int s;
        // take addr and compare it with
        for (s=0; s<nm; s++) // iterate through all mapped regions
            if (start[s] > addr) // find the region the addr belongs to
                break;
        if (s-- && addr < end[s]) // make sure the address was found and that it belongs to the mapped area
        {
#else

        if (find_region_idx(addr) != -1) {
#endif
            // if yes, use the address for hash calculation
            if (backtrace_unwinded((void*)addr, 0))
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

#else

uint64_t bthash(uint64_t size)
{
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

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
    {
        void *addr = sp[i];
        int s;
        // take addr and compare it with
        for (s=0; s<nm; s++) // iterate through all mapped regions
            if (start[s] > addr) // find the region the addr belongs to
                break;
        if (s-- && addr < end[s]) // make sure the address was found and that it belongs to the mapped area
        {
            // if yes, use the address for hash calculation
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

#endif
