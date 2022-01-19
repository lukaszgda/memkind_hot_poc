// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021 Intel Corporation. */

#include <memkind/internal/memkind_memtier.h>

#include <memkind/internal/tachanka.h>

#include <argp.h>
#include <assert.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <stdint.h>
#include <thread>
#include <map>
#include <vector>
#include <atomic>
#include <iomanip>
#include <random>
#include <memkind/internal/pebs.h>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unistd.h>
#include <limits.h>

#include "../test/zipf.h"


// static bool g_pushEvents=true;

static uint64_t clock_bench_time_ms() {
    struct timespec tv;
//     int ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv);
    int ret = clock_gettime(CLOCK_MONOTONIC, &tv);
    assert (ret == 0);
    return tv.tv_sec*1000 + tv.tv_nsec/1000000;
}

static uint64_t clock_bench_optional_time() {
#if ADJUST_FOR_RANKING_TOUCHES
    return clock_bench_time_ms();
#else
    return 0;
#endif
}


class Timestamp {
    uint64_t pebsTimestamp;
public:
    Timestamp() : pebsTimestamp(0) {}

    uint64_t GetPebsTimestamp() {
        return pebsTimestamp;
    }

    void AdvanceBy(double seconds) {
        const uint64_t ONE_SECOND = 1000000000;
        pebsTimestamp += (uint64_t)(ONE_SECOND*seconds);
    }
};

static Timestamp g_timestamp;
static uint64_t g_excludedTicks=0;
static uint64_t g_totalAccessesX0xFFFF=0;
static uint64_t g_totalMallocs=0;


#define CHECK_TEST 0

struct RunInfo {
    /* total operations: nTypes*typeIterations*testIterations
     *
     * for iterationsMajor:
     *      for nTypes:
     *          reallocate()
     *      for iterationsMinor::
     *          access()
     */
    size_t minSize;
    size_t nTypes;
    size_t iterationsMajor;
    size_t iterationsMinor;
    double s;
    memtier_policy_t policy;
    size_t pmem_dram_ratio;
};

struct ExecResults {
    uint64_t malloc_time_ms;
    uint64_t access_time_ms;
    std::vector<uint64_t> malloc_times_ms;
    std::vector<uint64_t> access_times_ms;
};

class Fence {
    bool ready=false;
    std::mutex m;
    std::condition_variable cv;

public:
    void Start() {
        std::lock_guard<std::mutex> lock(m);
        ready = true;
        cv.notify_all();
    }

    void Await() {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&]{ return ready; });
    }
};

#define TEST_ACCESS_RANDOM 0
#define TEST_ACCESS_RANDOM_INC 1
#define TEST_ACCESS_SEQ_CPY 2
#define TEST_ACCESS_SEQ_INC 3

#define TEST_ACCESS TEST_ACCESS_RANDOM_INC


#define IS_TEST_ACCESS_RANDOM ((TEST_ACCESS == TEST_ACCESS_RANDOM_INC) \
                                || (TEST_ACCESS == TEST_ACCESS_RANDOM_INC))
/// @warning: API is not thread safe, only one function per instance
/// should be called at once
class Loader  {

#if IS_TEST_ACCESS_RANDOM
    static std::random_device dev;
    std::mt19937 generator;
#endif

    std::shared_ptr<void> data1; //, data2;
    size_t dataSize;
    size_t iterations;
    size_t i=0;
    std::shared_ptr<std::thread> this_thread;
    std::atomic<bool> shouldContinue;
    uint64_t executionTimeMillis_=0;

    /// @return milliseconds per whole data access
    uint64_t GenerateAccessOnce_() {
        uint64_t timestamp_start = clock_bench_time_ms();
#if TEST_ACCESS == TEST_ACCESS_SEQ_CPY
        const size_t data_size_u64 = dataSize/sizeof(uint64_t);
        for (size_t it=0; it<iterations; ++it)
            for (size_t i=0; i<data_size_u64; i += CACHE_LINE_SIZE_U64) {
                static_cast<volatile uint64_t*>(data1.get())[i] =
                    static_cast<volatile uint64_t*>(data2.get())[i];
            }
#elif TEST_ACCESS == TEST_ACCESS_RANDOM
        for (size_t it=0; it<iterations; ++it)
            for (size_t i=0; i<dataSize/sizeof(uint64_t); i += CACHE_LINE_SIZE_U64) {
//                 size_t index = rand()%(dataSize/sizeof(uint64_t));
                std::uniform_int_distribution<uint64_t> distr(0, dataSize/sizeof(uint64_t));
                size_t index = distr(generator);
                static_cast<volatile uint64_t*>(data1.get())[index] =
                    static_cast<volatile uint64_t*>(data2.get())[index];
            }
#elif TEST_ACCESS == TEST_ACCESS_RANDOM_INC
        for (size_t it=0; it<iterations; ++it)
            for (size_t i=0; i<dataSize/sizeof(uint64_t); i += CACHE_LINE_SIZE_U64) {
//                 size_t index = rand()%(dataSize/sizeof(uint64_t));
                std::uniform_int_distribution<uint64_t> distr(0, dataSize/sizeof(uint64_t));
                size_t index = distr(generator);
                static_cast<uint64_t*>(data1.get())[index]++;
            }
#else
        const size_t data_size_u64 = dataSize/sizeof(uint64_t);
        for (size_t it=0; it<iterations; ++it)
            for (size_t i=CACHE_LINE_SIZE_U64, pi=0; i<data_size_u64; pi=i, i += CACHE_LINE_SIZE_U64) {
//                 static_cast<volatile uint64_t*>(data1.get())[i]
                    += static_cast<volatile uint64_t*>(data1.get())[pi];
            }
#endif
        return clock_bench_time_ms() - timestamp_start;
    }

public:
    Loader(std::shared_ptr<void> data1,
           /*std::shared_ptr<void> data2, */size_t data_size,
           size_t iterations) :
#if IS_TEST_ACCESS_RANDOM
          generator(dev()),
#endif
          data1(data1),
//           data2(data2),
          dataSize(data_size),
          iterations(iterations),
          shouldContinue(false) {}

    void PrepareOnce(std::shared_ptr<Fence> fence) {
        assert(!this_thread);
        this_thread = std::make_shared<std::thread>([this, fence](){
//             std::this_thread::sleep_for(std::chrono::seconds(10));
//             sleep(10);
            fence->Await();
            this->executionTimeMillis_ = this->GenerateAccessOnce_();
        });
    }

    uint64_t CollectResults() {
        assert(this_thread && "Collecting results of thread that was not started");
        this_thread->join();
        return this->executionTimeMillis_;
    }

    /// \todo hide from API ? TODO
    uint64_t GenerateAccessOnce() {
        return this->GenerateAccessOnce_();
    }
};

#if IS_TEST_ACCESS_RANDOM
std::random_device Loader::dev;
#endif

static std::shared_ptr<memtier_memory> create_memory(memtier_policy_t policy,
                                                     size_t pmem_dram_ratio) {
    struct memtier_builder *m_tier_builder =
        memtier_builder_new(policy);
    memtier_builder_add_tier(m_tier_builder, MEMKIND_DEFAULT, 1);
    memtier_builder_add_tier(m_tier_builder,
                                MEMKIND_REGULAR,
                                pmem_dram_ratio);
    auto memory =
        std::shared_ptr<memtier_memory>(
            memtier_builder_construct_memtier_memory(m_tier_builder),
            [](struct memtier_memory *m) {
                memtier_delete_memtier_memory(m);
            });
    memtier_builder_delete(m_tier_builder);

    return memory;
}


class LoaderCreator {
    size_t size, iterations;
    std::shared_ptr<memtier_memory> memory;
public:
    LoaderCreator(size_t size,
                  size_t iterations,
                  std::shared_ptr<memtier_memory> memory) :
        size(size), iterations(iterations), memory(memory) {}

    std::shared_ptr<Loader> CreateLoader() {
        void *allocated_memory1 = memtier_malloc(memory.get(), size);
//         void *allocated_memory2 = memtier_malloc(memory.get(), size);
        assert(allocated_memory1 && "memtier malloc failed");
//         assert(allocated_memory2 && "memtier malloc failed");
        std::shared_ptr<void> allocated_memory_ptr1(allocated_memory1,
                                                    memtier_free);
//         std::shared_ptr<void> allocated_memory_ptr2(allocated_memory2,
//                                                     memtier_free);

        return std::make_shared<Loader>(
            allocated_memory_ptr1, /*allocated_memory_ptr2,*/ size, iterations);
    }
};

/// @return malloc_time, access_time
static std::pair<uint64_t, uint64_t>
create_and_run_loaders(std::vector<LoaderCreator> &creators) {
    std::vector<std::shared_ptr<Loader>> loaders;
    auto fence = std::make_shared<Fence>();
    loaders.reserve(creators.size());
    // populate this->vector<...>
    uint64_t start_mallocs_timestamp = clock_bench_time_ms();
    for (auto &creator : creators) {
        loaders.push_back(creator.CreateLoader());
    }
    uint64_t malloc_time = clock_bench_time_ms() - start_mallocs_timestamp;
    for (auto &loader : loaders) {
        loader->PrepareOnce(fence);
    }
    fence->Start();
    uint64_t summed_access_time_ms = 0u;
    for (auto &loader : loaders) {
        summed_access_time_ms += loader->CollectResults();
    }

    return std::make_pair(malloc_time, summed_access_time_ms);
}

/// @param N number of elements
/// @return \sum_{n=1}^{N}(1/n^s)
static double calculate_zipf_denominator(int N, double s) {
    // brute force approach, might be optimised (?)
    // but it should not be necessary
    double ret = 0;
    for (int n=1; n<=N; ++n) {
        ret += 1.0/pow(n, s);
    }

    return ret;
}

/// Creates loader creators - each one holds paramters for a loader
///
/// The number of iterations for each loader is described by zipf distribution
///
/// Instead of generating random numbers that follow zipf distribution,
/// we create a population that strictly follows zipf distribution by hand
///
/// The zipf distribution is described by:
///     f(k, s, N) = \frac{1/k^s}{\sum_{n=1}^{N}(1/n^s)}
///
/// where:
///     - k: rank of element, starts with 1,
///     - s: distribution parameter,
///     - N: total number of elements
///
/// @param s zipf distribution parameter
static std::vector<LoaderCreator>
create_loader_creators (size_t minSize,
                        size_t nTypes,
                        size_t total_iterations,
                        double s,
                        memtier_policy_t policy,
                        size_t pmem_dram_ratio) {
    auto memory = create_memory(policy, pmem_dram_ratio);
    std::vector<LoaderCreator> creators;
    creators.reserve(nTypes);
    double zipf_denominator = calculate_zipf_denominator(nTypes, s);
    // create loader creators that share memory
    double probability_sum=0;
    for (size_t size = minSize, k=1u; size<minSize+nTypes; ++size, ++k) {
        // TODO create LoaderCreator of size size and of rank
        double probability = 1/pow(k, s)/zipf_denominator;
        probability_sum += probability;
        // TODO total_iterations*probability has no effect, requires debugging
        size_t iterations = (size_t)(total_iterations*probability);
        creators.push_back(LoaderCreator(size, iterations, memory));
    }
    assert(abs(probability_sum-1.0) < 0.0001 && "probabilities do not sum to 100% !");
    return creators;
}

/// @return duration of allocations and accesses in milliseconds
ExecResults run_test(const RunInfo &info) {

    std::vector<LoaderCreator> loader_creators
        = create_loader_creators(info.minSize,
                                 info.nTypes,
                                 info.iterationsMinor,
                                 info.s,
                                 info.policy,
                                 info.pmem_dram_ratio);

    std::vector<uint64_t> malloc_times;
    std::vector<uint64_t> access_times;
    malloc_times.reserve(info.iterationsMajor);
    access_times.reserve(info.iterationsMajor);

    ExecResults ret = {0};

    ret.access_times_ms.reserve(info.iterationsMajor);
    ret.malloc_times_ms.reserve(info.iterationsMajor);

    for (size_t major_it = 0; major_it < info.iterationsMajor; ++major_it) {
        auto [malloc_time, access_time] =
            create_and_run_loaders(loader_creators);
        ret.malloc_time_ms += malloc_time;
        ret.access_time_ms += access_time;
        ret.malloc_times_ms.push_back(malloc_time);
        ret.access_times_ms.push_back(access_time);
    }

    return ret;
}

void init_global_sys_info(void) {
    // 8 bytes in uint64_t
//     CACHE_LINE_SIZE_U64 = sysconf(LEVEL1_DCACHE_LINESIZE)/8;
}


// what needs to be done:
// 1. Benchmark all threads, not just one
// 2. Use loader to bench tests ACTION
// 3. re-allocate loaders
// 4. allocate loaders with specified policy
// 5. vary size of each loader - create separate types
// 6. benchmark separately - access times and allocation time

static void matmul(void);

int main(int argc, char *argv[])
{
    init_global_sys_info();
    size_t PMEM_TO_DRAM = 4;
    size_t LOADER_SIZE = 1024*1024*512; // half gigabyte
//     size_t LOADER_SIZE = 1024*512; // half megabyte
//     size_t LOADER_SIZE = 512; // half kilobyte
    // avoid interactions between manual touches and hardware touches
//     pebs_set_process_hardware_touches(false);

    matmul();

    assert(argc == 5 &&
        "Incorrect number of arguments specified, "
        "please specify 2 arguments: [static|hotness] [test_iterations] [type_iterations] [threads]"); // FIXME temporary
    // FIXME temporary arg parsing...
    memtier_policy_t policy = argv[1] == std::string("static") ?
        MEMTIER_POLICY_STATIC_RATIO
        : argv[1] == std::string("hotness") ?
            MEMTIER_POLICY_DATA_HOTNESS
            : MEMTIER_POLICY_MAX_VALUE;
    size_t test_iterations = atoi(argv[2]);
    size_t type_iterations = atoi(argv[3]);
    size_t THREADS = atoi(argv[4]);

    assert(policy != MEMTIER_POLICY_MAX_VALUE &&
        "please specify a known policy [hotness|static]");
    assert(THREADS>0 && "at least one thread is required");

    RunInfo info = {
        .minSize = LOADER_SIZE,
        .nTypes = THREADS,
        .iterationsMajor = test_iterations,
        .iterationsMinor = type_iterations,
        .s = 2.0, // arbitrary value, should it be read from command line?
        .policy = policy,
        .pmem_dram_ratio = 8,
    };

    ExecResults stats = run_test(info);

    std::cout << "Measured execution time [malloc | access]: ["
              << stats.malloc_time_ms << "|" << stats.access_time_ms
              << "]" << std::endl;

    std::cout << "Malloc times: ";
    for (uint64_t malloc_time_ms : stats.malloc_times_ms)
        std::cout << malloc_time_ms << ",";
    std::cout << std::endl;

    std::cout << "Access times: ";
    for (uint64_t access_time_ms : stats.access_times_ms)
        std::cout << access_time_ms << ",";
    std::cout << std::endl;

    matmul();

    return 0;
}

static void naive_matrix_multiply(int matrix_size, int mul_step,
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

static void matmul() {

    struct memtier_builder *m_builder;
    struct memtier_memory *m_tier_memory;
    {
        m_tier_memory = nullptr;
        m_builder = memtier_builder_new(MEMTIER_POLICY_DATA_HOTNESS);
    }

    const int MATRIX_SIZE = 512;
    const int MUL_STEP = 5;
    int OBJS_NUM = 3;

    // objects will be reallocated after N uses
    const int AGE_THRESHOLD = 10;
    const int LOOP_LEN = 1;
//     const int LOOP_LEN = 20 * OBJS_NUM;
    // start iteration of hotness validation
    const int LOOP_CHECK_START = 5 * OBJS_NUM;
    // compare sum of hotness between objects from DEPTH num of checks
    const int LOOP_CHECK_DEPTH = 10;
    // get object hotness every FREQ iterations
    const int LOOP_CHECK_FREQ = 10;

    int it, it2;

    // setup only DRAM tier
    int res = memtier_builder_add_tier(m_builder, MEMKIND_DEFAULT, 1);
//     ASSERT_EQ(0, res);
    // currently, adding only one tier is not supported
    // workaround: add two tiers, both dram
    res = memtier_builder_add_tier(m_builder, MEMKIND_REGULAR, 1);
//     ASSERT_EQ(0, res);
    m_tier_memory = memtier_builder_construct_memtier_memory(m_builder);
//     ASSERT_NE(nullptr, m_tier_memory);

    int mat_size = sizeof(double) * MATRIX_SIZE * MATRIX_SIZE;

    float accum_hotness[OBJS_NUM][LOOP_LEN] = {0};
    for (it = 0; it < OBJS_NUM; it++)
        for (it2 = 0; it2 < LOOP_LEN; it2++)
            accum_hotness[it][it2]=0;

    double** objs = (double**)memtier_malloc(m_tier_memory, OBJS_NUM * sizeof(double*));
    for (it = 0; it < OBJS_NUM; it++) {
        objs[it] = 0;
    }

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
//             ASSERT_NE(nullptr, dest_obj);

            // DEBUG
            //printf("remalloc %d, start %llx, end %llx\n", dest_obj_id,
            //    (long long unsigned int)(&objs[dest_obj_id][0]),
            //    (long long unsigned int)(&objs[dest_obj_id][MATRIX_SIZE * MATRIX_SIZE - 1]));
        }

// 	    naive_matrix_multiply(MATRIX_SIZE, MUL_STEP,
//             dest_obj, dest_obj, dest_obj);
//         for (size_t j=0; j<1000; ++j)
//             for (size_t i=0; i< MATRIX_SIZE*MATRIX_SIZE; i += CACHE_LINE_SIZE_U64)
//                 dest_obj[i] ++;

        Loader loader(std::shared_ptr<void>(dest_obj, [](void* addr){
            (void) addr;
        }), MATRIX_SIZE*MATRIX_SIZE, 100000);
        auto fence = std::make_shared<Fence>();
        std::thread([m_tier_memory, mat_size](){
            double *ndest_obj = (double*)memtier_malloc(m_tier_memory,
                MATRIX_SIZE*MATRIX_SIZE );
            Loader internal_loader(std::shared_ptr<void>(ndest_obj, [](void* addr){
                (void) addr;
            }), MATRIX_SIZE*MATRIX_SIZE, 100000);
            internal_loader.GenerateAccessOnce();
            memtier_free(ndest_obj);
        }).join();
//         loader.PrepareOnce(fence);
//         std::this_thread::sleep_for(std::chrono::seconds(15));
//         sleep(5);
//         fence->Start();
//         loader.CollectResults();
//         loader.GenerateAccessOnce();

        if (ready_to_validate == 0) {
            int num_allocated_objs = 0;
            for (int it2 = 0; it2 < OBJS_NUM; it2++) {
                if (objs[it2] != NULL)
                num_allocated_objs++;
            }

            if (num_allocated_objs == OBJS_NUM) {
                for (int it2 = 0; it2 < OBJS_NUM; it2++) {
                    accum_hotness[it2][it] = tachanka_get_obj_hotness(mat_size + it2 * sizeof(double));
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
                for (size_t it3 = 0; it3 < LOOP_CHECK_DEPTH; it3++) {
                    h0 += accum_hotness[it2 - 1][it - it3];
                    h1 += accum_hotness[it2][it - it3];
                }
//                 ASSERT_GE(h0, h1);
            }
        }

        // DEBUG
        //printf("dst %d\n", dest_obj_id);
        //fflush(stdout);

        // TODO check object hotness
        //
        // How the check for hotness should look like:
        //  1) sort all objects/types by their frequency
        //  2) calculate sum of all sizes
        //  3) hand-calculate which objects should be cold and which ones hot
        //  4) make a check in two loops: first loop for hot, second loop for cold
        //
        // Possible issues:
        //  1) pebs thread has not done its work; mitigation:
        //      a) wait (race condition-based mitigation)
        //      b) explicitly call pebs thread
        //  2) general race condition with pebs:
        //      i) cases:
        //          a) not enough: see point 1,
        //          b) too many times: old time window is gone,
        //          latest has 0 measurements
        //      ii) mitigation:
        //          a) explicitly call pebs, without separate thread
        //
        // For now, only "quickfix": make a test that's vulnerable to race condition
	}

    {
        memtier_builder_delete(m_builder);
        if (m_tier_memory) {
            memtier_delete_memtier_memory(m_tier_memory);
        }
    }
}
