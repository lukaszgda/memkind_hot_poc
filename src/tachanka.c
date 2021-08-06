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

/*static*/ critnib *hash_to_type, *addr_to_block;

int is_hot(uint64_t hash)
{
    return 1;
}

#define HOTNESS_MEASURE_WINDOW 1000000000ULL
#define MALLOC_HOTNESS 20

void register_block(uint64_t hash, void *addr, size_t size)
{
    struct ttype *t = critnib_get(hash_to_type, hash);
    if (!t) {
        t = &ttypes[__sync_fetch_and_add(&ntypes, 1)];
        if (t >= &ttypes[MAXTYPES])
            fprintf(stderr, "Too many distinct alloc types\n"), exit(1);
        t->size = size;
        t->hot_or_not = -2; // no time set
        if (critnib_insert(hash_to_type, hash, t, 0) == EEXIST) {
            t = critnib_get(hash_to_type, hash); // raced with another thread
            if (!t)
                fprintf(stderr, "Alloc type disappeared?!?\n"), exit(1);
        }
    }

    t->num_allocs++;
    t->total_size+= size;

    struct tblock *bl = &tblocks[__sync_fetch_and_add(&nblocks, 1)];
    if (bl >= &tblocks[MAXBLOCKS])
        fprintf(stderr, "Too many allocated blocks\n"), exit(1);

    bl->addr = addr;
    bl->size = size;
    bl->type = t - ttypes;
    critnib_insert(addr_to_block, (uintptr_t)addr, bl, 0);
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
    hash_to_type = critnib_new();
    addr_to_block = critnib_new();
}


// DEBUG

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

MEMKIND_EXPORT float get_obj_hotness(int size)
{
    for (int i = 0; i < 20; i++) {
        struct ttype* t = critnib_get_leaf(hash_to_type, i);

        if (t != NULL && t->size == size) {
             return t->f;
        }
    }

    return -1;
}
