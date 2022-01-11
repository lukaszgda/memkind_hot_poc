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

/// @warning: API is not thread safe, only one function per instance
/// should be called at once
class Loader  {
    std::shared_ptr<volatile void> data1, data2;
    size_t dataSize;
    size_t iterations;
    size_t i=0;
    std::shared_ptr<std::thread> this_thread;
    std::atomic<bool> shouldContinue;
    uint64_t executionTimeMillis_=0;

    /// @return milliseconds per whole data access
    uint64_t GenerateAccessOnce_() {
        uint64_t timestamp_start = clock_bench_time_ms();
        for (size_t it=0; it<iterations; ++it)
            for (size_t i=0; i<dataSize/sizeof(uint64_t); i += CACHE_LINE_SIZE_U64) {
                static_cast<volatile uint64_t*>(data1.get())[i] =
                    static_cast<volatile uint64_t*>(data2.get())[i];
            }

        return clock_bench_time_ms() - timestamp_start;
    }

public:
    Loader(std::shared_ptr<volatile void> data1,
           std::shared_ptr<volatile void> data2, size_t data_size,
           size_t iterations)
        : data1(data1),
          data2(data2),
          dataSize(data_size),
          iterations(iterations),
          shouldContinue(false) {}

    void PrepareOnce(std::shared_ptr<Fence> fence) {
        assert(!this_thread);
        this_thread = std::make_shared<std::thread>([this, fence](){
            fence->Await();
            this->executionTimeMillis_ = this->GenerateAccessOnce_();
        });
    }

    uint64_t CollectResults() {
        assert(this_thread && "Collecting results of thread that was not started");
        this_thread->join();
        return this->executionTimeMillis_;
    }
};

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
        void *allocated_memory2 = memtier_malloc(memory.get(), size);
        std::shared_ptr<void> allocated_memory_ptr1(allocated_memory1,
                                                    memtier_free);
        std::shared_ptr<void> allocated_memory_ptr2(allocated_memory2,
                                                    memtier_free);

        return std::make_shared<Loader>(
            allocated_memory_ptr1, allocated_memory_ptr2, size, iterations);
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

int main(int argc, char *argv[])
{
    init_global_sys_info();
    size_t PMEM_TO_DRAM = 4;
    size_t LOADER_SIZE = 1024*1024*512; // half gigabyte
    // avoid interactions between manual touches and hardware touches
    pebs_set_process_hardware_touches(false);

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
        .minSize = 512 * 1024 * 1024, // 512 MB per thread
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

    return 0;
}
