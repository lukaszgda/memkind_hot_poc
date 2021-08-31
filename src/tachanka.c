#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <memkind/internal/pebs.h>
#include <memkind/internal/bthash.h>
#include <memkind/internal/critnib.h>
#include <memkind/internal/tachanka.h>
#include <memkind/internal/ranking.h>

// #define MALLOC_HOTNESS      20u
#define MALLOC_HOTNESS      1u

#define MAXTYPES   1*1048576
#define MAXBLOCKS 16*1048576

// DEBUG

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

struct ttype ttypes[MAXTYPES];
struct tblock tblocks[MAXBLOCKS];

static int ntypes = 0;
static int nblocks = 0;
static int freeblock = -1;

// TODO (possibly) move elsewhere - make sure multiple rankings are supported!
static ranking_t *ranking;
static _Atomic double g_dramToTotalMemRatio=1.0;
/*static*/ critnib *hash_to_type, *addr_to_block;

#define ADD(var,x) __sync_fetch_and_add(&(var), (x))
#define SUB(var,x) __sync_fetch_and_sub(&(var), (x))

void register_block(uint64_t hash, void *addr, size_t size)
{
    struct ttype *t = critnib_get(hash_to_type, hash);
    if (!t) {
        t = &ttypes[__sync_fetch_and_add(&ntypes, 1)];
        if (t >= &ttypes[MAXTYPES])
            fprintf(stderr, "Too many distinct alloc types\n"), exit(1);
        t->size = size;
        t->timestamp_state = TIMESTAMP_NOT_SET;// registering block - only when firt one!
        if (critnib_insert(hash_to_type, hash, t, 0) == EEXIST) {
            t = critnib_get(hash_to_type, hash); // raced with another thread
            if (!t)
                fprintf(stderr, "Alloc type disappeared?!?\n"), exit(1);
        }
        t->n1=0u;
        t->n2=0u;
        t->touchCb = NULL;
        t->touchCbArg = NULL;
    }

    t->num_allocs++;
    t->total_size+= size;

    int fb, nf;
    do {
        fb = freeblock;
        if (fb == -1) {
            fb = __sync_fetch_and_add(&nblocks, 1);
            if (fb >= MAXBLOCKS)
                fprintf(stderr, "Too many allocated blocks\n"), exit(1);
            break;
        }
        nf = tblocks[fb].nextfree;
    } while (!__atomic_compare_exchange_n(&freeblock, &fb, nf, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    struct tblock *bl = &tblocks[fb];
    if (bl >= &tblocks[MAXBLOCKS])
        fprintf(stderr, "Too many allocated blocks\n"), exit(1);

    bl->addr = addr;
    bl->size = size;
    bl->type = t - ttypes;
    critnib_insert(addr_to_block, (uintptr_t)addr, bl, 0);
}

void realloc_block(void *addr, void *new_addr, size_t size)
{
    struct tblock *bl = critnib_remove(addr_to_block, (intptr_t)addr);
    if (!bl)
        return (void)fprintf(stderr, "Tried realloc a non-allocated block at %p\n", addr);

    bl->addr = new_addr;
    struct ttype *t = &ttypes[bl->type];
    SUB(t->total_size, bl->size);
    ADD(t->total_size, size);
    critnib_insert(addr_to_block, (uintptr_t)new_addr, bl, 0);
}

void unregister_block(void *addr)
{
    struct tblock *bl = critnib_remove(addr_to_block, (intptr_t)addr);
    if (!bl)
        return (void)fprintf(stderr, "Tried deallocating a non-allocated block at %p\n", addr);
    //printf("freeing block at %p size %zu\n", addr, bl->size);
    struct ttype *t = &ttypes[bl->type];
    SUB(t->num_allocs, 1);
    SUB(t->total_size, bl->size);
    bl->addr = 0;
    bl->size = 0;
    int blind = bl - tblocks;
    __atomic_exchange(&freeblock, &blind, &bl->nextfree, __ATOMIC_ACQ_REL);
}

MEMKIND_EXPORT Hotness_e tachanka_get_hotness_type(const void *addr)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (!bl || addr >= bl->addr + bl->size)
        return HOTNESS_NOT_FOUND;
    struct ttype *t = &ttypes[bl->type];
    if (ranking_is_hot(ranking, t))
        return HOTNESS_HOT;
    return HOTNESS_COLD;
}

MEMKIND_EXPORT double tachanka_get_hot_thresh(void)
{
    return ranking_get_hot_threshold(ranking);
}

MEMKIND_EXPORT Hotness_e tachanka_get_hotness_type_hash(uint64_t hash)
{
    Hotness_e ret = HOTNESS_NOT_FOUND;
    struct ttype *t = critnib_get(hash_to_type, hash);
    if (t) {
        if (ranking_is_hot(ranking, t))
            ret = HOTNESS_HOT;
        else
            ret = HOTNESS_COLD;
    }

    return ret;
}

#include "stdatomic.h"
#include "assert.h"

void touch(void *addr, __u64 timestamp, int from_malloc)
{
//     printf("touches tachanka start, timestamp: [%llu], from malloc [%d]\n", timestamp, from_malloc);
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (!bl || addr >= bl->addr + bl->size) {
//         printf("tachanka aborts [pointer]: [%p]\n", bl);
//         if (bl)
//             printf("tachanka aborts [bl->addr, bl->size, addr]: [%p, %lu, %p]\n", bl->addr, bl->size, addr);
        return;
    }
//     else
//     {
//         printf("tachanka touch for known area!\n");
//     }
    struct ttype *t = &ttypes[bl->type];
    // TODO - is this thread safeness needed? or best effort will be enough?
    //__sync_fetch_and_add(&t->accesses, 1);

    int hotness=1;
    if (from_malloc) {
        ranking_add(ranking, t); // first of all, add
        hotness=MALLOC_HOTNESS;
    }
    static atomic_uint_fast16_t counter=0;
    const uint64_t interval=1000;
    if (++counter > interval) {
        struct timespec t;
        int ret = clock_gettime(CLOCK_MONOTONIC, &t);
        if (ret != 0) {
            printf("ASSERT TOUCH COUNTER FAILURE!\n");
        }
        assert(ret == 0);
        printf("touch counter %lu hit, [seconds, nanoseconds]: [%ld, %ld]\n",
            interval, t.tv_sec, t.tv_nsec);
        counter=0u;
    }

    // TODO make decisions regarding thread-safeness
    // thread-safeness:
    //  - can we actually touch a removed structure?
    //  - is it a problem?
    // current solution: assert(FALSE)
    // future solution: ignore?
//     printf("touches tachanka, timestamp: [%llu]\n", timestamp);
    ranking_touch(ranking, t, timestamp, hotness);
}

void tachanka_init(double old_window_hotness_weight)
{
    read_maps();
    hash_to_type = critnib_new();
    addr_to_block = critnib_new();
    ranking_create(&ranking, old_window_hotness_weight);
}

MEMKIND_EXPORT void tachanka_set_dram_total_ratio(double ratio)
{
    if (ratio<0 || ratio>1)
        printf("Incorrect ratio [%f], exiting", ratio), exit(-1);
    g_dramToTotalMemRatio=ratio;
}

void tachanka_update_threshold(void)
{
    // TODO remove this!!!
    printf("wre: tachanka_update_threshold\n");
    // EOF TODO

    ranking_calculate_hot_threshold_dram_total(ranking, g_dramToTotalMemRatio);
}


void tachanka_destroy(void)
{
    ranking_destroy(ranking);
}

MEMKIND_EXPORT double tachanka_get_obj_hotness(int size)
{
    for (int i = 0; i < 20; i++) {
        struct ttype* t = critnib_get_leaf(hash_to_type, i);

        if (t != NULL && t->size == size) {
             return t->f;
        }
    }

    return -1;
}

MEMKIND_EXPORT double tachanka_get_addr_hotness(void *addr)
{
    double ret = -1;
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bl) {
        struct ttype *t = &ttypes[bl->type];
        ret = t->f;
    }
    return ret;
}

// MEMKIND_EXPORT double tachanka_set_touch_callback(void *addr, const char *name)
MEMKIND_EXPORT int tachanka_set_touch_callback(void *addr, tachanka_touch_callback cb, void* arg)
{
    int ret = -1;
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bl) {
        struct ttype *t = &ttypes[bl->type];

        ranking_set_touch_callback(ranking, cb, arg, t);
        ret=0;
    }
    return ret;
}
