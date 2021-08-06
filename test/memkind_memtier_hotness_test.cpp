// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021 Intel Corporation. */

#include <memkind/internal/memkind_memtier.h>

#include <random>
#include <thread>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "common.h"

#define MATRIX_SIZE 512
#define MUL_STEP 5
#define OBJS_NUM 3
#define LOOP_LEN 100
#define LOOP_CHECK 50
#define AGE_THRESHOLD 10

void naive_matrix_multiply(double *a, double *b, double *c) {
    double s;
    int i,j,k;

    for(i=0;i<MATRIX_SIZE;i++) {
        for(j=0;j<MATRIX_SIZE;j+=MUL_STEP) {
            a[i * MATRIX_SIZE + j]=(double)i*(double)j;
            b[i * MATRIX_SIZE + j]=(double)i/(double)(j+5);
        }
    }

    for(j=0;j<MATRIX_SIZE;j++) {
        for(i=0;i<MATRIX_SIZE;i++) {
            s=0;
            for(k=0;k<MATRIX_SIZE;k+=MUL_STEP) {
                s+=a[i * MATRIX_SIZE + k]*b[k * MATRIX_SIZE + j];
            }
            c[i * MATRIX_SIZE + j] = s;
        }
    }

    s = 0.0;
    for(i=0;i<MATRIX_SIZE;i++) {
        for(j=0;j<MATRIX_SIZE;j+=MUL_STEP) {
            s+=c[i * MATRIX_SIZE + j];
        }
    }

    return;
}

class MemkindMemtierKindTest: public ::testing::Test
{
private:
    void SetUp()
    {}
    void TearDown()
    {}
};

class MemkindMemtierHotnessTest: public ::testing::Test
{
protected:
    struct memtier_builder *m_builder;
    struct memtier_memory *m_tier_memory;

private:
    void SetUp()
    {
        m_tier_memory = nullptr;
        m_builder = memtier_builder_new(MEMTIER_POLICY_DATA_HOTNESS);
        ASSERT_NE(nullptr, m_builder);
    }

    void TearDown()
    {
        memtier_builder_delete(m_builder);
        if (m_tier_memory) {
            memtier_delete_memtier_memory(m_tier_memory);
        }
    }
};

TEST_F(MemkindMemtierHotnessTest, test_tier_two_kinds)
{
    int res = memtier_builder_add_tier(m_builder, MEMKIND_DEFAULT, 1);
    ASSERT_EQ(0, res);
    res = memtier_builder_add_tier(m_builder, MEMKIND_REGULAR, 1);
    ASSERT_EQ(0, res);
    m_tier_memory = memtier_builder_construct_memtier_memory(m_builder);
    ASSERT_NE(nullptr, m_tier_memory);
}

TEST_F(MemkindMemtierHotnessTest, test_matmul)
{
    int res = memtier_builder_add_tier(m_builder, MEMKIND_DEFAULT, 1);
    ASSERT_EQ(0, res);
    m_tier_memory = memtier_builder_construct_memtier_memory(m_builder);
    ASSERT_NE(nullptr, m_tier_memory);

    int mat_size = sizeof(double) * MATRIX_SIZE * MATRIX_SIZE;

    double* objs[OBJS_NUM] = {0};
    int ages[OBJS_NUM];
    float accum_hotness[OBJS_NUM] = {0};
    int it;

    // fill frequency array
    // objects with lower ID are more frequent
    // 0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 4 ....
    const int FREQ_ARRAY_LEN = (OBJS_NUM * OBJS_NUM + OBJS_NUM) / 2;
    int freq[FREQ_ARRAY_LEN];
    int aa = 0, bb = 0;
    for (it = 0; it < FREQ_ARRAY_LEN; it++) {
        freq[it] = aa;
        aa++;
        if (aa > bb) {
            bb++;
            aa = 0;
        }
    }

    // set age of each object to AGE_THRESHOLD to reallocate it immediately
    for (it = 0; it < OBJS_NUM; it++) {
        ages[it] = AGE_THRESHOLD;
    }

    int sel = 0;
    int ready_to_validate = 0;
	for (it = 0; it < LOOP_LEN; it++) {

        // select src1, src2 and dest objects
        sel++;
        sel = sel % FREQ_ARRAY_LEN;

        int dest_obj_id = freq[sel];
        double* dest_obj = objs[dest_obj_id];

        // each object has an age - if it goes above AGE_THRESHOLD
        // object is reallocated
        ages[dest_obj_id]++;
        if (ages[dest_obj_id] > AGE_THRESHOLD) {
            ages[dest_obj_id] = 0;

            memtier_free(dest_obj);
            objs[dest_obj_id] = (double*)memtier_malloc(m_tier_memory,
                mat_size + dest_obj_id * sizeof(double));
            dest_obj = objs[dest_obj_id];
            ASSERT_NE(nullptr, dest_obj);

            // DEBUG
            //printf("remalloc %d, start %llx, end %llx\n", dest_obj_id,
            //    (long long unsigned int)(&objs[dest_obj_id][0]),
            //    (long long unsigned int)(&objs[dest_obj_id][MATRIX_SIZE * MATRIX_SIZE - 1]));
        }

	    naive_matrix_multiply(dest_obj, dest_obj, dest_obj);

        if (ready_to_validate == 0) {
            int num_allocated_objs = 0;
            for (int it2 = 0; it2 < OBJS_NUM; it2++) {
                if (objs[it2] != NULL)
                num_allocated_objs++;
            }

            if (num_allocated_objs == OBJS_NUM) {
                for (int it2 = 0; it2 < OBJS_NUM; it2++) {
                    accum_hotness[it2] += get_obj_hotness(mat_size + it2 * sizeof(double));
                }

                if (it > LOOP_CHECK) {
                    ready_to_validate = 1;
                }
            }
        } else {
            float h0 = accum_hotness[0];
            float h1 = accum_hotness[1];
            float h2 = accum_hotness[2];

            ASSERT_GE(h0, h1);
            ASSERT_GE(h1, h2);
        }

        // DEBUG
        //printf("dst %d\n", dest_obj_id);
        //fflush(stdout);
	}
}
