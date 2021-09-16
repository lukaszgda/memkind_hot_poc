#pragma once

#include "stdatomic.h"
#include "stdbool.h"
#include "stdint.h"
#include "stddef.h"

typedef struct lq_entry {
    // metadata
    atomic_uint metadata_state; // necessary
    // data to store
    void *data;
} lq_entry_t;

typedef struct lq_buffer {
    // TODO specify which one needs atomicity
    // algorithm: first
    // cycle of life:
    // request write     used ++, if possible
    //      |
    //      V
    //      write
    //      |
    //      V
    //      post write      unavailableRead--, // alternative: availableRead++,
    //      |
    //      V
    //      request read    unavailableRead++, if possible // alternative: availableRead--, if possible (would have to be signed!)
    //      |
    //      V
    //      read
    //      |
    //      V
    //      post read    used --
    //      |
    //      V
    //      request write (all around...)
    //
    //      initial values: used = 0, unavailableRead = size
    //      why such strange name (unavailableRead) - we can try increment and check if > size, but we cannot try decrement and check < 0 (size is unsigned); it would be possible if we changed variable to signed
    lq_entry_t *entries;
    void *data;
    size_t size;
    size_t entrySize;
    atomic_size_t head;
    atomic_size_t tail;
    atomic_size_t used; /// elements that are between: request write and post read
    atomic_size_t unavailableRead; /// elements that are **NOT** between: post_write and request_read; other words: **ARE** between request read and post write; this number cannot exceed size!
} lq_buffer_t;

extern void lq_init(lq_buffer_t *buff, size_t entry_size, size_t entries);

extern void lq_destroy(lq_buffer_t *buff);

// simple wrapper
/// @return
///     - true: success (@p out was written),
///     - false: no entry available for pop
extern bool lq_pop(lq_buffer_t *buff, void *out);

// simple wrapper
/// @return
///     - true: success (@p in was copied onto queue),
///     - false: no place available on buffer
extern bool lq_push(lq_buffer_t *buff, void *in);
