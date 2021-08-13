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

#define MALLOC_HOTNESS      20u
#define DRAM_TO_PMEM_RATIO  (1./8.)

#define MAXTYPES   1*1048576
#define MAXBLOCKS 16*1048576
struct ttype ttypes[MAXTYPES];
struct tblock tblocks[MAXBLOCKS];

static int ntypes = 0;
static int nblocks = 0;
static int freeblock = -1;

// TODO (possibly) move elsewhere - make sure multiple rankings are supported!
ranking_t *ranking;
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
        t->timestamp_state = TIMESTAMP_NOT_SET;
        if (critnib_insert(hash_to_type, hash, t, 0) == EEXIST) {
            t = critnib_get(hash_to_type, hash); // raced with another thread
            if (!t)
                fprintf(stderr, "Alloc type disappeared?!?\n"), exit(1);
        }
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

Hotness_e tachanka_is_hot(const void *addr)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (!bl || addr >= bl->addr + bl->size)
        return HOTNESS_NOT_FOUND;
    struct ttype *t = &ttypes[bl->type];
    if (ranking_is_hot(ranking, t))
        return HOTNESS_HOT;
    return HOTNESS_COLD;
}

void touch(void *addr, __u64 timestamp, int from_malloc)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (!bl)
        return;
    if (addr >= bl->addr + bl->size)
        return;

    struct ttype *t = &ttypes[bl->type];
    // TODO - is this thread safeness needed? or best effort will be enough?
    //__sync_fetch_and_add(&t->accesses, 1);

    if (from_malloc) {
        ranking_add(ranking, t); // first of all, add
    }
    // TODO make decisions regarding thread-safeness
    // thread-safeness:
    //  - can we actually touch a removed structure?
    //  - is it a problem?
    // current solution: assert(FALSE)
    // future solution: ignore?
    ranking_touch(ranking, t, timestamp, MALLOC_HOTNESS);
}

void tachanka_init(void)
{
    read_maps();
    hash_to_type = critnib_new();
    addr_to_block = critnib_new();
    ranking_create(&ranking);
}

void tachanka_update_threshold(void)
{
    ranking_calculate_hot_threshold_dram_pmem(ranking, DRAM_TO_PMEM_RATIO);
}


void tachanka_destroy(void)
{
    ranking_destroy(ranking);
}

// DEBUG

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

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
