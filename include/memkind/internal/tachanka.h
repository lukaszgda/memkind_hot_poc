#pragma once
#include "stdint.h"
#include "asm-generic/int-ll64.h"

typedef enum TimestampState {
    TIMESTAMP_NOT_SET,
    TIMESTAMP_INIT,
    TIMESTAMP_INIT_DONE,
} TimestampState_t;

void register_block(uint64_t hash, void *addr, size_t size);
void unregister_block(void *addr);
void realloc_block(void *addr, void *new_addr, size_t size);
void *new_block(size_t size);
void touch(void *addr, __u64 timestamp, int from_malloc);
void tachanka_init(void);

struct ttype {
    size_t size;
    int num_allocs; // TODO
    int total_size; // TODO

    __u64 t2;   // start of previous measurement window
    __u64 t1;   // start of current window
    __u64 t0;   // timestamp of last processed data

    int n2;   // num of access in prev window
    int n1;   // num of access in current window

    float f;  // frequency
    TimestampState_t timestamp_state;
};

struct tblock
{
    void *addr;
    ssize_t size;
    int type;
    int nextfree; // can reuse one of other fields
};
