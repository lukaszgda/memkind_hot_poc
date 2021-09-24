#include <include/memkind/internal/memkind_memtier.h>

#include <stdbool.h>
#include <stdlib.h>
#include <execinfo.h>
#include <threads.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>

// SIMD instructions
#include "mmintrin.h"
#include "xmmintrin.h"
#include "emmintrin.h"
#include "immintrin.h"


#define ARRAYSZ(x) (sizeof(x)/sizeof((x)[0]))

static void __attribute__((aligned(64))) *start[1024];
static void __attribute__((aligned(64))) *end[1024];
static int nm;
static void *stack0;
static void *stack_start, *stack_end;
static void *pthread_start, *pthread_end; // hax!

static thread_local uint64_t stack_size;
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

        //printf("map: start %p end %p\n", start[i], end[i]);
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
// TODO end condition is kind of weak;
// apart from that, there are multiple entries for libpthread - currently,
// only the last one is taken into account, all others are ignored


static bool backtrace_unwinded(const void* addr, size_t idx) {
    return /* addr == __libc_csu_init || */ /* idx > 2 || */ (addr >= pthread_start && addr < pthread_end);
}

#if CUSTOM_BACKTRACE

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

#if LIB_BINSEARCH
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
#elif SIMD_INSTRUCTIONS
/// @pre  addr < start[0] || addr >= end[nm-1]
/// @return 1 on successful find, -1 on failure
static bool check_region(void *addr)
{
    if (addr < start[0] || addr >= end[nm-1])
        return false;
    bool found=false;
    int simd_iterations = (nm/8)*8;
    int simd_x3_iterations = (nm/24)*24; // pipeline calculations for peak flops
    // TODO handle the nasty bool casting hack better (corner cases - overflow casted to false!)
//     int regular_iterations = nm%8;
    // TODO try to optimize
    // load addr to register

    void  __attribute__((aligned(64)))*extended_addr[]={
        //
        addr,
        addr,
        addr,
        addr,
        addr,
        addr,
        addr,
        addr,
    };
//     __m512i addr_reg =_mm_load_si512((__m512i*)extended_addr);
    __m512i *addr_reg_ptr =(__m512i*)extended_addr;
    for (int i0=0, i1=8, i2=16; i0<simd_x3_iterations; i0+=24, i1+=24, i2+=24) {
        // load 1
//         __m512i a = _mm_load_si512((__m512i*)&start[i]);
//         __m512i b = _mm_load_si512((__m512i*)&end[i]);
        __m512i *a0 = (__m512i*)&start[i0];
        __m512i *b0 = (__m512i*)&end[i0];
        __m512i *a1 = (__m512i*)&start[i1];
        __m512i *b1 = (__m512i*)&end[i1];
        __m512i *a2 = (__m512i*)&start[i2];
        __m512i *b2 = (__m512i*)&end[i2];
        // load 2
        __mmask8 mask_gt0 = _mm512_cmpge_epu64_mask (*addr_reg_ptr, *a0);
        __mmask8 mask_all0 = _mm512_mask_cmplt_epu64_mask (mask_gt0, *addr_reg_ptr, *b0);
        __mmask8 mask_gt1 = _mm512_cmpge_epu64_mask (*addr_reg_ptr, *a1);
        __mmask8 mask_all1 = _mm512_mask_cmplt_epu64_mask (mask_gt1, *addr_reg_ptr, *b1);
        __mmask8 mask_gt2 = _mm512_cmpge_epu64_mask (*addr_reg_ptr, *a2);
        __mmask8 mask_all2 = _mm512_mask_cmplt_epu64_mask (mask_gt2, *addr_reg_ptr, *b2);
        // unload
//         uint8_t mask_unloaded = mask_all; // TODO find corresponding "store" operation
//         _mm_store
        found = ( found || mask_all0 || mask_all1 || mask_all2);
    }
    for (int i=simd_x3_iterations; i<simd_iterations; i+=8) {
        // load 1
//         __m512i a = _mm_load_si512((__m512i*)&start[i]);
//         __m512i b = _mm_load_si512((__m512i*)&end[i]);
        __m512i *a = (__m512i*)&start[i];
        __m512i *b = (__m512i*)&end[i];
        // load 2
        __mmask8 mask_gt = _mm512_cmpge_epu64_mask (*addr_reg_ptr, *a);
        __mmask8 mask_all = _mm512_mask_cmplt_epu64_mask (mask_gt, *addr_reg_ptr, *b);
        // unload
//         uint8_t mask_unloaded = mask_all; // TODO find corresponding "store" operation
//         _mm_store
        found = ( found || mask_all );
    }
    // handle the remainder
    for (int i=simd_iterations; i<nm; ++i) {
        found = ( found || (addr >= start[i] && addr < end[i]) );
    }
    // TODO loop over nm and perform the operation below
    // addr >= start && addr < end // this operation, but in vector terms
    // __mmask8 _mm512_mask_cmpge_epu64_mask (__mmask8 k1, __m512i a, __m512i b)
    // __mmask8 _mm512_cmpgt_epu64_mask (__m512i a, __m512i b)
    // __mmask8 _mm512_mask_cmplt_epu64_mask (__mmask8 k1, __m512i a, __m512i b)
    // __mmask8 _mm512_mask_cmplt_epu64_mask (__mmask8 k1, __m512i a, __m512i b)

    return found;
}
#endif

static bool is_on_stack(const void* ptr) {
    return ptr >= stack_start && ptr < stack_end;
}

#if REDUCED_STACK_SEARCH
static const size_t max_searchable_stack_size=16u;// TODO move
#else
static const size_t max_searchable_stack_size=160u;// TODO move
#endif
static size_t max_diff=0;

static void align_up(void **ptr) {
    *ptr += (8-((uint64_t)*ptr)%8); // 8-byte boundary
}

static void align_down(void **ptr) {
    *ptr -= (((uint64_t)*ptr)%8); // 8-byte boundary
}

static inline uint64_t update_hash(uint64_t h, uint64_t k, uint64_t M, uint64_t R) {
    k *= M;
    k ^= k >> R;
    k *= M;
    h ^= k;
    h *= M;
    return h;
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
        stack_size = diff;
        if (diff > max_searchable_stack_size) {
            if (diff>max_diff) {
                max_diff = diff;
            }

            // TODO actually, stack may grow/decrease; we should take the most
            // "recent" elements, not the ones with the highest values
            stack_bottom = stack_top - max_searchable_stack_size;
        }
        align_up(&stack_bottom);
        align_down(&stack_top);
    } else {
        p1 = p2 = NULL;
    }
}

uint64_t bthash(uint64_t size)
{
    // MurmurHash2 by Austin Appleby, public domain.
    const uint64_t M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;
    uint64_t h = size ^ M;

    h = update_hash(h, stack_size, M, R);

    // can we directly obtain stack pointer?
//     void *stock_ptr = __builtin_frame_address(0);
//     for (void **sp = __builtin_frame_address(1); sp != stack0; sp++)
#if STACK_RANGE
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
#if LIB_BINSEARCH
        if (find_region_idx(addr) != -1) {
#elif SIMD_INSTRUCTIONS
        if (check_region(addr)) {
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
            h = update_hash(h, k, M, R);
        }
    }
#if FINALIZE_HASH
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

#if FINALIZE_HASH
    // Enable if we want "random" hash values.
    h ^= h >> R;
    h *= M;
    h ^= h >> R;
#endif

    return h;
}

#endif
