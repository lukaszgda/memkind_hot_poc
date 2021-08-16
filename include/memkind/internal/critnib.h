/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2018, Intel Corporation */

#ifndef CRITNIB_H
#define CRITNIB_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct critnib;
typedef struct critnib critnib;

struct critnib *critnib_new(const uint64_t *leaves, int leaf_stride);
void critnib_delete(struct critnib *c);

int critnib_insert(struct critnib *c, int leaf);
int critnib_remove(struct critnib *c, uint64_t key);
int critnib_get(struct critnib *c, uint64_t key);
int critnib_find_le(struct critnib *c, uint64_t key);
void critnib_iter(struct critnib *c, int (*func)(int leaf));

#ifdef __cplusplus
}
#endif

#endif
