// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021 Intel Corporation. */

#include <memkind/internal/memkind_memtier.h>

#include <random>
#include <thread>
#include <vector>
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
#include "zipf.h"

void naive_matrix_multiply(int matrix_size, int mul_step,
                                            double *a, double *b, double *c) {
    double s;
    int i,j,k;

    for(i=0;i<matrix_size;i++) {
        for(j=0;j<matrix_size;j+=mul_step) {
            a[i * matrix_size + j]=(double)i*(double)j;
            b[i * matrix_size + j]=(double)i/(double)(j+5);
        }
    }

    for(j=0;j<matrix_size;j++) {
        for(i=0;i<matrix_size;i++) {
            s=0;
            for(k=0;k<matrix_size;k+=mul_step) {
                s+=a[i * matrix_size + k]*b[k * matrix_size + j];
            }
            c[i * matrix_size + j] = s;
        }
    }

    s = 0.0;
    for(i=0;i<matrix_size;i++) {
        for(j=0;j<matrix_size;j+=mul_step) {
            s+=c[i * matrix_size + j];
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

class MemkindMemtierHotnessTest: public ::testing::Test,
      public ::testing::WithParamInterface<int>
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

TEST_P(MemkindMemtierHotnessTest, test_matmul)
{
    const int MATRIX_SIZE = 512;
    const int MUL_STEP = 5;
    int OBJS_NUM = GetParam();

    // objects will be reallocated after N uses
    const int AGE_THRESHOLD = 10;
    const int LOOP_LEN = 20 * OBJS_NUM;
    // start iteration of hotness validation
    const int LOOP_CHECK_START = 5 * OBJS_NUM;
    // compare sum of hotness between objects from DEPTH num of checks
    const int LOOP_CHECK_DEPTH = 10;
    // get object hotness every FREQ iterations
    const int LOOP_CHECK_FREQ = 10;

    int it, it2;

    // setup only DRAM tier
    int res = memtier_builder_add_tier(m_builder, MEMKIND_DEFAULT, 1);
    ASSERT_EQ(0, res);
    m_tier_memory = memtier_builder_construct_memtier_memory(m_builder);
    ASSERT_NE(nullptr, m_tier_memory);

    int mat_size = sizeof(double) * MATRIX_SIZE * MATRIX_SIZE;

    float accum_hotness[OBJS_NUM][LOOP_LEN] = {0};
    for (it = 0; it < OBJS_NUM; it++)
        for (it2 = 0; it2 < LOOP_LEN; it2++)
            accum_hotness[it][it2]=0;

    double** objs = (double**)malloc(OBJS_NUM * sizeof(double*));
    for (it = 0; it < OBJS_NUM; it++) {
        objs[it] = 0;
    }

    // fill frequency array
    // objects with lower ID are more frequent
    // 0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 4 ....
    /*
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
    */

    // fill frequency array using zipf distribution
    std::random_device rd;
    std::mt19937 gen(rd());
    zipf_distribution<> zipf(OBJS_NUM);
    const int FREQ_ARRAY_LEN = LOOP_LEN;
    int freq[LOOP_LEN];
    for (it = 0; it < LOOP_LEN; it++) {
        freq[it] = zipf(gen) - 1;
    }

    // set age of each object to AGE_THRESHOLD to reallocate it immediately
    int ages[OBJS_NUM];
    for (it = 0; it < OBJS_NUM; it++) {
        ages[it] = AGE_THRESHOLD;
    }

    int sel = 0;
    int ready_to_validate = 0;
    int check_freq = 0;
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

	    naive_matrix_multiply(MATRIX_SIZE, MUL_STEP,
            dest_obj, dest_obj, dest_obj);

        if (ready_to_validate == 0) {
            int num_allocated_objs = 0;
            for (int it2 = 0; it2 < OBJS_NUM; it2++) {
                if (objs[it2] != NULL)
                num_allocated_objs++;
            }

            if (num_allocated_objs == OBJS_NUM) {
                for (int it2 = 0; it2 < OBJS_NUM; it2++) {
                    accum_hotness[it2][it] = get_obj_hotness(mat_size + it2 * sizeof(double));
                }

                if (it > LOOP_CHECK_START) {
                    ready_to_validate = 1;
                }
            }
        } else {
            check_freq ++;
            if (check_freq < LOOP_CHECK_FREQ)
                continue;

            check_freq = 0;
            for (int it2 = 1; it2 < OBJS_NUM; it2++) {
                float h0 = 0, h1 = 0;
                for (int it3 = 0; it3 < LOOP_CHECK_DEPTH; it3++) {
                    h0 += accum_hotness[it2 - 1][it - it3];
                    h1 += accum_hotness[it2][it - it3];
                }
                ASSERT_GE(h0, h1);
            }
        }

        // DEBUG
        //printf("dst %d\n", dest_obj_id);
        //fflush(stdout);
	}
}

INSTANTIATE_TEST_CASE_P(numObjsParam, MemkindMemtierHotnessTest,
                        ::testing::Values(3, 20));
