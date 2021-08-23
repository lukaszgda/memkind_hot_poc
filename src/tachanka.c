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
#include <memkind/internal/bigary.h>

// #define MALLOC_HOTNESS      20u
#define MALLOC_HOTNESS      1u
#define DRAM_TO_PMEM_RATIO  (1./8.)

// DEBUG
#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif


#define MAXTYPES   1*1048576
#define MAXBLOCKS 16*1048576
struct ttype *ttypes;
struct tblock *tblocks;

static int ntypes = 0;
static int nblocks = 0;
static int freeblock = -1;

// TODO (possibly) move elsewhere - make sure multiple rankings are supported!
static ranking_t *ranking;
/*static*/ critnib *hash_to_type, *addr_to_block;
static bigary ba_ttypes, ba_tblocks;

#define ADD(var,x) __sync_fetch_and_add(&(var), (x))
#define SUB(var,x) __sync_fetch_and_sub(&(var), (x))

void register_block(uint64_t hash, void *addr, size_t size)
{
    struct ttype *t;
    int nt = critnib_get(hash_to_type, hash);
    if (nt == -1) {
        nt = __sync_fetch_and_add(&ntypes, 1);
        t = &ttypes[nt];
        if (nt >= MAXTYPES)
            fprintf(stderr, "Too many distinct alloc types\n"), exit(1);
        t->hash = hash;
        t->size = size;
        t->timestamp_state = TIMESTAMP_NOT_SET;
        if (critnib_insert(hash_to_type, nt) == EEXIST) {
            nt = critnib_get(hash_to_type, hash); // raced with another thread
            if (nt == -1)
                fprintf(stderr, "Alloc type disappeared?!?\n"), exit(1);
            t = &ttypes[nt];
        }
    } else {
        t = &ttypes[nt];
    }

    t->n1=0u;
    t->n2=0u;
    t->touchCb = NULL;
    t->touchCbArg = NULL;
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
    if (fb >= MAXBLOCKS)
        fprintf(stderr, "Too many allocated blocks\n"), exit(1);

    bl->addr = addr;
    bl->size = size;
    bl->type = nt;
    critnib_insert(addr_to_block, fb);
}

void realloc_block(void *addr, void *new_addr, size_t size)
{
    int bln = critnib_remove(addr_to_block, (intptr_t)addr);
    if (bln == -1)
        return (void)fprintf(stderr, "Tried realloc a non-allocated block at %p\n", addr);
    struct tblock *bl = &tblocks[bln];

    bl->addr = new_addr;
    struct ttype *t = &ttypes[bl->type];
    SUB(t->total_size, bl->size);
    ADD(t->total_size, size);
    critnib_insert(addr_to_block, bln);
}

void unregister_block(void *addr)
{
    int bln = critnib_remove(addr_to_block, (intptr_t)addr);
    if (bln == -1)
        return (void)fprintf(stderr, "Tried deallocating a non-allocated block at %p\n", addr);
    struct tblock *bl = &tblocks[bln];
    struct ttype *t = &ttypes[bl->type];
    SUB(t->num_allocs, 1);
    SUB(t->total_size, bl->size);
    bl->addr = 0;
    bl->size = 0;
    __atomic_exchange(&freeblock, &bln, &bl->nextfree, __ATOMIC_ACQ_REL);
}

MEMKIND_EXPORT Hotness_e tachanka_get_hotness_type(const void *addr)
{
    int bln = critnib_find_le(addr_to_block, (uintptr_t)addr);

    if (bln >= 0 || addr >= tblocks[bln].addr + tblocks[bln].size)
        return HOTNESS_NOT_FOUND;
    struct ttype *t = &ttypes[tblocks[bln].type];
    if (ranking_is_hot(ranking, t))
        return HOTNESS_HOT;
    return HOTNESS_COLD;
}

MEMKIND_EXPORT Hotness_e tachanka_get_hotness_type_hash(uint64_t hash)
{
    Hotness_e ret = HOTNESS_NOT_FOUND;
    int nt = critnib_get(hash_to_type, hash);
    if (nt != -1) {
        struct ttype *t = &ttypes[nt];
        if (ranking_is_hot(ranking, t))
            ret = HOTNESS_HOT;
        else
            ret = HOTNESS_COLD;
    }

    return ret;
}

void touch(void *addr, __u64 timestamp, int from_malloc)
{
    int bln = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bln == -1)
        return;
    struct tblock *bl = &tblocks[bln];
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

void tachanka_init(double old_window_hotness_weight)
{
    bigary_init(&ba_tblocks, BIGARY_DRAM, 0);
    tblocks = ba_tblocks.area;
    bigary_init(&ba_ttypes, BIGARY_DRAM, 0);
    ttypes = ba_ttypes.area;
    read_maps();
    addr_to_block = critnib_new((uint64_t*)tblocks, sizeof(tblocks[0]) / sizeof(uint64_t));
    hash_to_type = critnib_new((uint64_t*)ttypes, sizeof(ttypes[0]) / sizeof(uint64_t));
    ranking_create(&ranking, old_window_hotness_weight);
}

void tachanka_update_threshold(void)
{
    ranking_calculate_hot_threshold_dram_pmem(ranking, DRAM_TO_PMEM_RATIO);
}

void tachanka_destroy(void)
{
    ranking_destroy(ranking);
}

static int _size;
static double _hotness;

static int size_hotness(int nt)
{
    if (ttypes[nt].size == _size) {
        _hotness = ttypes[nt].f;
        return 1;
    }
    return 0;
}

MEMKIND_EXPORT double tachanka_get_obj_hotness(int size)
{
    _size = size;
    _hotness = -1;
    critnib_iter(hash_to_type, size_hotness);
    return _hotness;
}

MEMKIND_EXPORT double tachanka_get_addr_hotness(void *addr)
{
    double ret = -1;
    int bln = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bln >= 0) {
        struct ttype *t = &ttypes[tblocks[bln].type];
        ret = t->f;
    }
    return ret;
}

// MEMKIND_EXPORT double tachanka_set_touch_callback(void *addr, const char *name)
MEMKIND_EXPORT int tachanka_set_touch_callback(void *addr, tachanka_touch_callback cb, void* arg)
{
    int ret = -1;
    int bln = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bln>=0) {
        struct ttype *t = &ttypes[tblocks[bln].type];

        ranking_set_touch_callback(ranking, cb, arg, t);
        ret=0;
    }
    return ret;
}
