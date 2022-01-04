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



#define ADJUST_FOR_RANKING_TOUCHES 0
#define TOUCH_PROBABILITY 0.2

/// number of u64 on single cache line
// #define CACHE_LINE_SIZE_U64 16 // 16*8 == 128 TODO read cache line size from some config?
// size_t CACHE_LINE_SIZE_U64=8; // 8*8 == 64 TODO read cache line size from some config?

// Interface for data generation
class DataGenerator {
public:
    virtual char *Generate(size_t size) = 0;
};

class DataSink {
public:
    virtual void Sink(char c) = 0;
};

/// The purpose of this class is to avoid optimising out calculations
/// by providing a simple way to use data
class SummatorSink : public DataSink {
    size_t sum=0;
public:
    void Sink(char c) {
        sum += (size_t)c;
    }
    ~SummatorSink() {
        std::cout << "Sink sum: " << sum << std::endl;
    }
};

/// The purpose of this class is to avoid optimising out calculations
/// by providing a simple way to use data
class RandomInitializedGenerator : public DataGenerator {

    std::vector<char> data;
    size_t idx;

public:

    RandomInitializedGenerator(size_t len) : idx(0) {
        data.reserve(len);
        std::random_device rd;
//         std::uniform_int_distribution<char> uniform_char(0, 0xFF);
        std::uniform_int_distribution<char> uniform_char;
        for (size_t i=0; i<len; ++i) {
            data.push_back(uniform_char(rd));
        }
    }

    char *Generate(size_t size) {
        assert(size < data.size() && "Generator's buffer is too small");
        ++idx;
        if (size+idx > data.size())
            idx = 0;
        return data.data()+idx;
    }
};

// What now: add two child classes that allocate data using different policies
// data should be allocated from memory,
// single memory should be created for all types

// how to account for operations common for static ratio
//
// TODO right now, there are singletons in memkind_memtier
// and actually creating multiple memories would use common structures
// underneath, making all results worthless
//
// THIS SHOULD BE FIXED!

static bool g_pushEvents=true;

static uint64_t clock_bench_time() {
    struct timespec tv;
//     int ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv);
    int ret = clock_gettime(CLOCK_MONOTONIC, &tv);
    assert (ret == 0);
    return tv.tv_sec*1000 + tv.tv_nsec/1000000;
}

static uint64_t clock_bench_optional_time() {
#if ADJUST_FOR_RANKING_TOUCHES
    return clock_bench_time();
#else
    return 0;
#endif
}


class Timestamp {
    uint64_t pebsTimesamp;
public:
    Timestamp() : pebsTimesamp(0) {}

    uint64_t GetPebsTimestamp() {
        return pebsTimesamp;
    }

    void AdvanceBy(double seconds) {
        const uint64_t ONE_SECOND = 1000000000;
        pebsTimesamp += (uint64_t)(ONE_SECOND*seconds);
    }
};

static Timestamp g_timestamp;
static uint64_t g_excludedTicks=0;
static uint64_t g_totalAccessesX0xFFFF=0;
static uint64_t g_totalMallocs=0;


class AllocationType {
    char *data;
    size_t size;
    double accessProbability;   /// should be distributed 0-1
    /// probability that, given that access is performed,
    /// a manual touch is triggered
    const double touchFrequency;   /// should be distributed 0-1
    double cumulatedTouchCoeff;    /// should be distributed 0-1
    std::shared_ptr<memtier_memory> memory;

    void GenerateTouch() {
        static uint16_t accesses_0xFFFF=0;
        if (((++accesses_0xFFFF) & 0xffff) == 0)
            ++g_totalAccessesX0xFFFF;
        cumulatedTouchCoeff += touchFrequency;
        if(cumulatedTouchCoeff>1) {
            uint64_t start = clock_bench_optional_time();
            while (cumulatedTouchCoeff>1) {
                // touch - very important stuff
                EventEntry_t entry;
                entry.type = EVENT_TOUCH;
                entry.data.touchData.address = data;
                entry.data.touchData.timestamp = g_timestamp.GetPebsTimestamp();
                if (g_pushEvents)
                    (void)tachanka_ranking_event_push(&entry);
                --cumulatedTouchCoeff;
            }
            uint64_t end = clock_bench_optional_time();
            uint64_t excluded_span = end-start;
            g_excludedTicks += excluded_span;
        }
    }

public:

    AllocationType(size_t size,
                   double accessProbability,
                   double touchFrequency,
                   std::shared_ptr<memtier_memory> memory) :
        data(nullptr),
        size(size),
        accessProbability(accessProbability),
        touchFrequency(touchFrequency),
        cumulatedTouchCoeff(0.0),
        memory(memory) {}

    void Reallocate() {
        ++g_totalMallocs;
        if (data)
            memtier_free(data);
        data = static_cast<char*>(memtier_malloc(memory.get(), size));
        memset(data, 0, size);
    }

    void GenerateSet(DataGenerator &gen) {
        assert(data && "data not initialized!");
        (void)memcpy(data, gen.Generate(size), size);
        GenerateTouch();
    }

    void GenerateGet(DataSink &sink) {
        for (size_t i=0; i < size; ++i)
            sink.Sink(data[i]);
         GenerateTouch();
    }

    size_t GetSize() {
        return size;
    }

    double GetAccessProbability() {
        return accessProbability;
    }

    AllocationType operator = (const AllocationType &other) = delete;

    AllocationType(const AllocationType &other) :
        data(nullptr),
        size(other.size),
        accessProbability(other.accessProbability),
        touchFrequency(other.touchFrequency),
        cumulatedTouchCoeff(0),
        memory(other.memory)
    {}
};

class AllocationTypeFactory {
    size_t maxSize, minSize, prob_coeff; // TODO upgrade prob_coeff
    std::shared_ptr<memtier_memory> memory; // TODO initialize memory

public:

    AllocationTypeFactory(
        size_t minSize, size_t maxSize, memtier_policy_t policy,
        size_t pmem_dram_ratio) :
            maxSize(maxSize), minSize(minSize) {
        assert(maxSize > minSize);
        struct memtier_builder *m_tier_builder = memtier_builder_new(policy);

        memtier_builder_add_tier(m_tier_builder, MEMKIND_DEFAULT, 1);
        memtier_builder_add_tier(m_tier_builder,
                                 MEMKIND_REGULAR,
                                 pmem_dram_ratio);
        memory =
            std::shared_ptr<memtier_memory>(
                memtier_builder_construct_memtier_memory(m_tier_builder),
                [](struct memtier_memory *m) {
                    memtier_delete_memtier_memory(m);
                });
        memtier_builder_delete(m_tier_builder);
    }

    AllocationType CreateType() {
//         TODO optimize it - create what can be created in constructor
        std::random_device rd;
        std::mt19937 gen(rd());
        zipf_distribution<> zipf_size(maxSize-minSize);
        double max_prob=20;
        zipf_distribution<> zipf_prob((size_t)max_prob); // round max_prob down
        // increase alloc size granularirty and prevent objects with "0" size
        size_t size = zipf_size(gen)+minSize;
        double probability = zipf_prob(gen)/max_prob;

        return AllocationType(size, probability, TOUCH_PROBABILITY, memory);
    }

    size_t GetMaxSize() {
        return maxSize;
    }
};

class AccessType {
    size_t PRE_GENERATED_SIZE=8; // nof bytes in U64
    double getPercentage;
    std::random_device rd;
    std::shared_ptr<std::mt19937> gen;// TODO fixme
    std::shared_ptr<std::uniform_int_distribution<uint64_t>> dist;
    std::vector<double> generated;
    size_t generatedIdx=PRE_GENERATED_SIZE;

    void GenerateMultiple_() {
        // this micro-optimisation is aimed at reducing
        // mersenne_twister_engine load on cpu
        uint64_t random_u64 = (*dist)(*gen);
        for (size_t i=0; i<PRE_GENERATED_SIZE; ++i) {
            generated[i]=(random_u64&0xFF)/(double)0xFF;
            random_u64 >>= 8;
        }
        generatedIdx = 0;
    }

    double GenerateSingle_() {
        if (generatedIdx >= PRE_GENERATED_SIZE) {
            GenerateMultiple_();
            assert(generatedIdx == 0);
        }
        return generated[generatedIdx++];
    }

public:
    enum class Type {
        GET,
        SET,
        NONE
    };

    AccessType(double get_percentage) :
            getPercentage(get_percentage), generated(PRE_GENERATED_SIZE) {
        gen = std::make_shared<std::mt19937>(rd());
        dist = std::make_shared<
            std::uniform_int_distribution<uint64_t>>(0, 0xFFFFFFFFFFFFFFFF);
    }

    Type Generate(double access_probability) {
        double val_access = GenerateSingle_();
        if (val_access <= access_probability) {
            // generate random variable once, use twice!
            double val = val_access/access_probability;
            return val > getPercentage ? Type::SET : Type::GET;
        }
        return Type::NONE;
    }
};

#define CHECK_TEST 0

struct RunInfo {
    /* total operations: nTypes*typeIterations*testIterations
     *
     * for testIterations:
     *      for nTypes:
     *          reallocate()
     *      for typeIterations:
     *          for nTypes:
     *              access()
     */
    size_t nTypes;
    size_t typeIterations;
    size_t testIterations;
};

struct ExecResults {
    uint64_t threadMillis;
    uint64_t systemMillis;
    uint64_t totalSize;
    uint64_t nofTypes;
    uint64_t nofBlocks;
};

/// @return duration of allocations and accesses in milliseconds
ExecResults run_test(AllocationTypeFactory &factory, const RunInfo &info) {
    // TODO handle somehow max number of types

    std::vector<AllocationType> types;
    types.reserve(info.nTypes);
    size_t total_types_size=0;


    for (size_t i=0; i<info.nTypes; ++i) {
        types.push_back(factory.CreateType());
        total_types_size += types.back().GetSize();
#if CHECK_TEST
        std::cout
            << "Type [size/probability] : [" << types.back().GetSize()
            << "/" << types.back().GetAccessProbability() << "]" << std::endl;
#endif
    }

    AccessType accessType(0.8);
#if CHECK_TEST
    const int ITERATIONS = 1000;
    // TEST GetVsSetAccess
    size_t gets=0, sets=0, nones=0;
    for (int i=0; i<ITERATIONS; ++i) {
        switch (accessType.Generate(0.6)) {
            case AccessType::Type::GET:
                ++gets;
                break;
            case AccessType::Type::SET:
                ++sets;
            case AccessType::Type::NONE:
                ++nones;
                break;
        }
    }
    std::cout
        << "gets/sets/nones|total: " << gets << "/" << sets << "/" << nones << "|" << ITERATIONS
        << std::endl;
#endif
    // all data printed

    // try generating accesses
    // TODO when reallocs?????
    // weird formula - make it scale nicely (reduce dispersion in run times)
//     size_t TEST_ITERATIONS = 5; //+50/info.iterations;
    size_t TEST_ITERATIONS = info.testIterations;
    size_t PER_TYPE_ITERATIONS = info.typeIterations;
//     size_t TYPES = 3000;
    SummatorSink sink;
    RandomInitializedGenerator gen(factory.GetMaxSize()*2);

    auto start = std::chrono::system_clock::now();
    uint64_t start_thread_millis = clock_bench_time();

    for (size_t i=0; i<TEST_ITERATIONS; ++i) {
        // reallocate all types
        for (auto &type : types)
            type.Reallocate();
        // touch all types TODO ideally, they should be touched at random...
        for (size_t j = 0; j < PER_TYPE_ITERATIONS; ++j) {
            g_timestamp.AdvanceBy(10.0/PER_TYPE_ITERATIONS);
            for (auto &type : types) {
                // TODO generate access
                switch (accessType.Generate(type.GetAccessProbability())) {
                    case AccessType::Type::GET:
                        type.GenerateGet(sink);
                        break;
                    case AccessType::Type::SET:
                        type.GenerateSet(gen);
                        break;
                    case AccessType::Type::NONE:
                        break;
                }
            }
            // TODO realloc
        }
    }
    auto end = std::chrono::system_clock::now();
    uint64_t timespan_thread_millis = clock_bench_time() - start_thread_millis;

    using namespace std::chrono;
    uint64_t execution_time_millis =
        duration_cast<milliseconds>(end-start).count();

    ExecResults ret;
    ret.systemMillis = execution_time_millis;
    ret.threadMillis = timespan_thread_millis;
    ret.totalSize = total_types_size;
    ret.nofTypes = info.nTypes;
    ret.nofBlocks = types.size();

    return ret;
}

class Loader  {
    std::shared_ptr<volatile void> data1, data2;
    size_t dataSize;
    size_t i=0;
    std::shared_ptr<std::thread> this_thread;
    std::atomic<bool> shouldContinue;

    void GenerateAccessOnce_() {
        for (size_t i=0; i<dataSize/sizeof(uint64_t); i += CACHE_LINE_SIZE_U64) {
//         i = (i+CACHE_LINE_SIZE_U64)%(dataSize/sizeof(uint64_t));
            static_cast<volatile uint64_t*>(data1.get())[i] =
                static_cast<volatile uint64_t*>(data2.get())[i];
        }
    }

    void GenerateUntilDoneAsync_() {
        if (!this_thread) {
            this_thread = std::make_shared<std::thread>([&](){
                while (shouldContinue) {
                    GenerateAccessOnce_();
                }
            });
        }
    }

public:
    Loader(std::shared_ptr<volatile void> data1,
           std::shared_ptr<volatile void> data2, size_t data_size) :
                data1(data1), data2(data2), dataSize(data_size),
                shouldContinue(true) {
        GenerateUntilDoneAsync_();
    }

    void Join() {
        shouldContinue = false;
        if(this_thread)
            this_thread->join();
        this_thread = nullptr;
    }
};

class LoaderThreadFactory {
    // for didtribuing allocations between dram and pmem
    std::shared_ptr<memtier_memory> memory;
public:
    LoaderThreadFactory(size_t pmem_dram_ratio) {
        // load is necessary for both: pmem and dram,
        // preferrably both at correct ratio
        // this should be taken care of when allocating memory
        struct memtier_builder *m_tier_builder =
            memtier_builder_new(MEMTIER_POLICY_STATIC_RATIO);
        memtier_builder_add_tier(m_tier_builder, MEMKIND_DEFAULT, 1);
        memtier_builder_add_tier(m_tier_builder,
                                 MEMKIND_REGULAR,
                                 pmem_dram_ratio);
        memory =
            std::shared_ptr<memtier_memory>(
                memtier_builder_construct_memtier_memory(m_tier_builder),
                [](struct memtier_memory *m) {
                    memtier_delete_memtier_memory(m);
                });
        memtier_builder_delete(m_tier_builder);
    }

    std::shared_ptr<Loader> CreateLoaderThread(size_t size) {
        void *allocated_memory1 = memtier_malloc(memory.get(), size);
        void *allocated_memory2 = memtier_malloc(memory.get(), size);
        std::shared_ptr<void> allocated_memory_ptr1(allocated_memory1,
                                                    memtier_free);
        std::shared_ptr<void> allocated_memory_ptr2(allocated_memory2,
                                                    memtier_free);
        return std::make_shared<Loader>(allocated_memory_ptr1, allocated_memory_ptr2, size);
  }
};

class MemoryLoadGenerator {
    std::atomic<bool> shouldWork;
    std::mutex threadsMutex;
    std::vector<std::shared_ptr<Loader>> threads;

    LoaderThreadFactory factory;

    void AddThread_(std::shared_ptr<Loader> &thread) {
        std::lock_guard<std::mutex> guard(threadsMutex);
        threads.push_back(thread);
    }

    std::shared_ptr<Loader> SpawnThread_(size_t size) {
        return factory.CreateLoaderThread(size);
    }

    /// join one thread
    /// @return false when there were no threads to join, false otherwise
    bool JoinOne_() {
        bool joined = false;
        std::shared_ptr<Loader> thread_to_join;
        std::unique_lock<std::mutex> guard(threadsMutex);
        if (threads.size() > 0) {
            thread_to_join = threads.back();
            threads.pop_back();
            joined = true;
        }
        guard.unlock();
        if (thread_to_join)
            thread_to_join->Join();

        return joined;
    }

public:

    MemoryLoadGenerator(size_t pmem_dram_ratio) : factory(pmem_dram_ratio) {}

    void SpawnThread(size_t size) {
        auto thread = SpawnThread_(size);
        AddThread_(thread);
    }

    void JoinAll() {
        while (JoinOne_());
    }
    ~MemoryLoadGenerator() {
        JoinAll();
    }
};

void init_global_sys_info(void) {
    // 8 bytes in uint64_t
//     CACHE_LINE_SIZE_U64 = sysconf(LEVEL1_DCACHE_LINESIZE)/8;
}

int main(int argc, char *argv[])
{
    init_global_sys_info();
    size_t PMEM_TO_DRAM = 8;
    size_t LOADER_SIZE = 1024*1024*512; // half gigabyte
    // avoid interactions between manual touches and hardware touches
    pebs_set_process_hardware_touches(false);
    RunInfo info;

    // hardcoded test constants
    info.nTypes = 15;

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


    const size_t LOADER_THREADS=THREADS-1;

    MemoryLoadGenerator generator(PMEM_TO_DRAM);

    for (size_t i=0; i<LOADER_THREADS; ++i) {
        generator.SpawnThread(LOADER_SIZE);
    }

//     size_t MIN_SIZE=1024*1024*50; // 50 MB
    size_t MIN_SIZE=256; // 256 B
    AllocationTypeFactory factory(MIN_SIZE, MIN_SIZE+32, policy, PMEM_TO_DRAM);

    info.testIterations = test_iterations;
    info.typeIterations = type_iterations;

    if (policy != MEMTIER_POLICY_DATA_HOTNESS)
        g_pushEvents=false;

    ExecResults stats = run_test(factory, info);

    std::cout
        << "Measured execution time [millis_thread|millis_thread_adjusted|millis_system]: ["
        << stats.threadMillis << "|"
        << (stats.threadMillis - g_excludedTicks)
        << "|" << stats.systemMillis << "]" << std::endl
        << "Stats [accesses_per_malloc|average_size]: ["
        << 0xFFFF*((double)g_totalAccessesX0xFFFF)/((double)g_totalMallocs) << "|"
        << stats.totalSize/(double)stats.nofTypes << "]" << std::endl
        << "Total size:" << stats.totalSize << std::endl;

    return 0;
}
