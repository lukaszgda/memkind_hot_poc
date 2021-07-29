#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <memkind/internal/bthash.h>
#include <memkind/internal/critnib.h>

#define MAXBLOCKS 16*1048576
static struct tblock
{
    void *addr;
    size_t size;
    long accesses;
    int next;
} tblocks[MAXBLOCKS];
static int nblocks = 0;

static critnib *hash_to_block, *addr_to_block;

int is_hot(uint64_t hash)
{
    return 1;
}

void register_block(uint64_t hash, void *addr, size_t size)
{
    struct tblock *bl = &tblocks[__sync_fetch_and_add(&nblocks, 1)];
    if (bl >= &tblocks[MAXBLOCKS])
        fprintf(stderr, "Too many allocated blocks\n"), exit(1);
    critnib_insert(hash_to_block, hash, bl, 0);
    critnib_insert(addr_to_block, (uintptr_t)addr, bl, 0);
}

void touch(void *addr)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (!bl)
        return;
    if (addr >= bl->addr + bl->size)
        return;
    __sync_fetch_and_add(&bl->accesses, 1);
}

void tachanka_init(void)
{
    read_maps();
    hash_to_block = critnib_new();
    addr_to_block = critnib_new();
}
