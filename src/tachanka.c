#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <memkind/internal/pebs.h>
#include <memkind/internal/bthash.h>
#include <memkind/internal/critnib.h>
#include <memkind/internal/tachanka.h>

#define MAXTYPES   1*1048576
#define MAXBLOCKS 16*1048576
struct ttype ttypes[MAXTYPES];
struct tblock tblocks[MAXBLOCKS];

static int ntypes = 0;
static int nblocks = 0;
static int freeblock = -1;

/*static*/ critnib *hash_to_type, *addr_to_block;

int is_hot(uint64_t hash)
{
    return 1;
}

#define ADD(var,x) __sync_fetch_and_add(&(var), (x))
#define SUB(var,x) __sync_fetch_and_sub(&(var), (x))

#define HOTNESS_MEASURE_WINDOW 1000000000ULL
#define MALLOC_HOTNESS 20

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
        t->hot_or_not = -2; // no time set
        if (critnib_insert(hash_to_type, nt) == EEXIST) {
            nt = critnib_get(hash_to_type, hash); // raced with another thread
            if (nt == -1)
                fprintf(stderr, "Alloc type disappeared?!?\n"), exit(1);
            t = &ttypes[nt];
        }
    } else {
        t = &ttypes[nt];
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
        t->n2 += MALLOC_HOTNESS;
    } else {
        t->t0 = timestamp;
        if (t->hot_or_not == -2) {
            t->t2 = timestamp;
            t->hot_or_not = -1;
        }
    }

    // check if type is ready for classification
    if (t->hot_or_not < 0) {
        t->n2 ++; // TODO - not thread safe is ok?

        // check if data is measured for time enough to classify hotness
        if ((t->t0 - t->t2) > HOTNESS_MEASURE_WINDOW) {
            // TODO - classify hotness
            t->hot_or_not = 1;
            t->t1 = t->t0;
        }
    } else {
        t->n1 ++; // TODO - not thread safe is ok?
        if ((t->t0 - t->t1) > HOTNESS_MEASURE_WINDOW) {
            // move to next measurement window
            float f2 = (float)t->n2 * t->t2 / (t->t2 - t->t0);
            float f1 = (float)t->n1 * t->t1 / (t->t2 - t->t0);
            t->f = f2 * 0.3 + f1 * 0.7; // TODO weighted sum or sth else?
            t->t2 = t->t1;
            t->t1 = t->t0;
            t->n2 = t->n1;
            t->n1 = 0;
        }
    }
}

void tachanka_init(void)
{
    read_maps();
    addr_to_block = critnib_new((uint64_t*)tblocks, sizeof(tblocks[0]) / sizeof(uint64_t));
    hash_to_type = critnib_new((uint64_t*)ttypes, sizeof(ttypes[0]) / sizeof(uint64_t));
}


// DEBUG

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

static int _size;
static float _hotness;

static int size_hotness(int nt)
{
    if (ttypes[nt].size == _size) {
        _hotness = ttypes[nt].f;
        return 1;
    }
    return 0;
}

MEMKIND_EXPORT float get_obj_hotness(int size)
{
    _size = size;
    _hotness = -1;
    critnib_iter(hash_to_type, size_hotness);
    return _hotness;
}
