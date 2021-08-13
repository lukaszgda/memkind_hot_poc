#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"
#include "asm-generic/int-ll64.h"

typedef enum TimestampState {
    TIMESTAMP_NOT_SET,
    TIMESTAMP_INIT,
    TIMESTAMP_INIT_DONE,
} TimestampState_t;

typedef enum Hotness {
    HOTNESS_HOT,
    HOTNESS_COLD,
    HOTNESS_NOT_FOUND,
} Hotness_e;

void register_block(uint64_t hash, void *addr, size_t size);
void unregister_block(void *addr);
void realloc_block(void *addr, void *new_addr, size_t size);
void *new_block(size_t size);
void touch(void *addr, __u64 timestamp, int from_malloc);
void tachanka_init(void);
void tachanka_destroy(void);
void tachanka_update_threshold(void);
double tachanka_get_obj_hotness(int size);
double tachanka_get_addr_hotness(void *addr);
Hotness_e tachanka_is_hot(const void *addr);

struct ttype {
    size_t size;
    int num_allocs; // TODO
    int total_size; // TODO

    __u64 t2;   // start of previous measurement window
    __u64 t1;   // start of current window
    __u64 t0;   // timestamp of last processed data

    int n2;   // num of access in prev window
    int n1;   // num of access in current window

    double f;  // frequency
    TimestampState_t timestamp_state;
};

struct tblock
{
    void *addr;
    ssize_t size;
    int type;
    int nextfree; // can reuse one of other fields
};

#ifdef __cplusplus
}
#endif
