#pragma once
// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021 Intel Corporation. */

#include "stdint.h"

void read_maps(void);
uint64_t bthash(uint64_t size);
void bthash_set_stack_range(void *p1, void *p2);
