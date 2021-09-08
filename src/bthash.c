#include "stdbool.h"
#include "stdlib.h"
#include "execinfo.h"
#include "threads.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CUSTOM_BACKTRACE
#define STACK_RANGE

#define ARRAYSZ(x) (sizeof(x)/sizeof((x)[0]))

static void *start[1024], *end[1024];
static int nm;
static void *stack0;
static void *stack_start, *stack_end;
static void *pthread_start, *pthread_end; // hax!

static thread_local void* stack_bottom=NULL; // TODO should probably be initialized to sth else
/// @pre stack_top >= stack_bottom
static thread_local void* stack_top=NULL; // TODO should probably be initialized to sth else

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
static const size_t max_searchable_stack_size=150u;// TODO move
static size_t max_diff=0;

static void align_up(void **ptr) {
    *ptr += (8-((uint64_t)*ptr)%8); // 8-byte boundary
}

static void align_down(void **ptr) {
    *ptr -= (((uint64_t)*ptr)%8); // 8-byte boundary
}

void bthash_set_stack_range(void *p1, void *p2) {
    if (p1 && p2) {
        if (p1>=p2) {
            stack_top = p1;
            stack_bottom = p2;
        } else {
            stack_top = p2;
            stack_bottom = p1;
        }
        size_t diff = ((uint64_t)stack_top)-((uint64_t)stack_bottom);
        if (diff > max_searchable_stack_size) {
            if (diff>max_diff) {
                max_diff = diff;
            }

            // TODO actually, stack may grow/decrease; we should take the most "recent" elements, not the ones with the highest values
            stack_bottom = stack_top-max_searchable_stack_size;
            // TODO only temporary ???
//             printf("slashing bthash stack range [bottom -> top, size]: [%p -> %p, %lu]\n", stack_bottom, stack_top, diff);
        }
        align_up(&stack_bottom);
        align_down(&stack_top);
    } else {
        p1 = p2 = NULL;
    }
}

uint64_t bthash(uint64_t size)
{
    volatile static uint64_t call_counter=0;
    call_counter++;
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

    // can we directly obtain stack pointer?
//     void *stock_ptr = __builtin_frame_address(0);
//     for (void **sp = __builtin_frame_address(1); sp != stack0; sp++)
#ifdef STACK_RANGE
    for (void **sp = stack_bottom; sp != stack_top; sp++)
#else
    for (void **sp = __builtin_frame_address(0); sp != stack0; sp++)
#endif
    {
//         volatile static uint64_t sp_counter=0;
//
//         -----
//         static thread_local bool assert_in_progress=false;
//         if (assert_in_progress) {
//             bthash has called itself recursively
//             this can only be caused by backtrace -> malloc -> bthash
//             backtrace -> malloc call occurs only when dynamic library is loaded
//             in such case, we return hash "0" for dynamic library
//             TODO this is a workaround and should probably be handled
//             in a smarter way
//             return 0; // valid hash - for a dynamic library
//         }
//         assert_in_progress=true;
//         assert(is_on_stack(sp));
//         assert_in_progress=false;


        // ---
//         assert(is_on_stack(sp));
//         sp_counter++;
        void *addr=*sp; // dereference value at stack; assume that the dereferenced value is a void pointer
#ifdef LIB_BINSEARCH
        if (find_region_idx(addr) != -1) {
#else
        int s;
        // take addr and compare it with
        for (s=0; s<nm; s++) // iterate through all mapped regions
            if (start[s] > addr) // find the region the addr belongs to
                break;
        if (s-- && addr < end[s]) // make sure the address was found and that it belongs to the mapped area
        {
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
