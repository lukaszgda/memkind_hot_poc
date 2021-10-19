#pragma once
#include "stdlib.h" // size_t
#include "pthread.h"
#include "sys/mman.h"

#define BIGARY_DRAM -1, MAP_ANONYMOUS|MAP_PRIVATE

#ifdef __cplusplus
#define restrict __restrict__
#endif

struct bigary
{
    void *area;
    size_t declared;
    size_t top;
    int fd;
    int flags;
    pthread_mutex_t enlargement;
};
typedef struct bigary bigary;

extern void bigary_init(bigary *restrict ba, int fd, int flags, size_t max);
extern void bigary_alloc(bigary *restrict ba, size_t top);
extern void bigary_destroy(bigary *restrict ba);
