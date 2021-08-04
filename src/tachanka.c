#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <memkind/internal/pebs.h>
#include <memkind/internal/bthash.h>
#include <memkind/internal/critnib.h>

#define MAXBLOCKS 16*1048576
struct tblock {
    void *addr;
    size_t size;

    __u64 t2;   // start of previous measurement window
    __u64 t1;   // start of current window
    __u64 t0;   // timestamp of last processed data
    float f2;   // num of access in prev window
    float f1;   // num of access in current window
    int hot_or_not; // -1 - not enough data, 0 - cold, 1 - hot
    int parent;
} tblocks[MAXBLOCKS];

static int nblocks = 0;

static critnib *hash_to_block, *addr_to_block;

int is_hot(uint64_t hash)
{
    return 1;
}

#define HOTNESS_MEASURE_WINDOW 100000

void register_block(uint64_t hash, void *addr, size_t size)
{
    struct tblock *bl = &tblocks[__sync_fetch_and_add(&nblocks, 1)];
    if (bl >= &tblocks[MAXBLOCKS])
        fprintf(stderr, "Too many allocated blocks\n"), exit(1);

    bl->addr = addr;
    bl->size = size;

    struct tblock *pbl = critnib_get(hash_to_block, hash);
    if (pbl)
        bl->parent = pbl - tblocks;
    else {
        bl->parent = -1;
        critnib_insert(hash_to_block, hash, bl, 0);
    }

    critnib_insert(addr_to_block, (uintptr_t)addr, bl, 0);
}

void touch(void *addr, __u64 timestamp)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (!bl)
        return;
    if (addr >= bl->addr + bl->size)
        return;
    if (bl->parent != -1)
        bl = &tblocks[bl->parent];

    // TODO - is this thread safeness needed? or best effor will be enough?
    //__sync_fetch_and_add(&bl->accesses, 1);

    bl->t0 = timestamp;

    // check if type needs classification
    if (bl->hot_or_not == -1) {
        bl->f2 ++; // TODO - not thread safe is ok?

        // check if data is measured for time enough to classify hotness
        if ((bl->t0 - bl->t2) > HOTNESS_MEASURE_WINDOW) {
            // TODO
            //classify_hotness(hi);
            bl->t1 = bl->t0;
        }
    } else {
        bl->f1 ++; // TODO - not thread safe is ok?
        if ((bl->t0 - bl->t1) > HOTNESS_MEASURE_WINDOW) {
            // move to next measurement window
            float f2 = bl->f2 * bl->t2 / (bl->t2 - bl->t0);
            float f1 = bl->f1 * bl->t1 / (bl->t2 - bl->t0);
            bl->f2 = f2 + f1; // TODO we could use weighted sum here
            bl->t2 = bl->t1;
            bl->t1 = bl->t0;
            bl->f1 = 0;
        }
    }
}

void tachanka_init(void)
{
    read_maps();
    hash_to_block = critnib_new();
    addr_to_block = critnib_new();
}
