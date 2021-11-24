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
#include "../test/zipf.h"

class counter_bench_alloc;

using Benchptr = std::unique_ptr<counter_bench_alloc>;
struct BenchArgs {
    Benchptr bench;
    size_t thread_no;
    size_t run_no;
    size_t iter_no;
    bool test_tiering;
};

struct RunRet {
    RunRet(double average_time, double average_dram_to_total_ratio) :
        averageTime(average_time),
        averageDramToTotalRatio(average_dram_to_total_ratio) {}
    double averageTime;
    double averageDramToTotalRatio;
    std::vector<double> singleRatios;
};

class counter_bench_alloc
{
public:
    RunRet run(BenchArgs& arguments) const
    {
        std::chrono::time_point<std::chrono::system_clock> start, end;
        start = std::chrono::system_clock::now();

        // calculated as promiles in size_t in order to
        // support atomic increments (at the price of accuracy)
        std::atomic<size_t> average_ratio_promile(0);

        std::vector<double> single_ratios;
        single_ratios.reserve(arguments.run_no);

        if (arguments.thread_no == 1) {
            for (size_t r = 0; r < arguments.run_no; ++r) {
                double single_run_ratio = single_run(arguments);
                single_ratios.push_back(single_run_ratio);
                average_ratio_promile +=
                    static_cast<size_t>(single_run_ratio*1000);
            }
        } else {
            std::vector<std::thread> vthread(arguments.thread_no);
            for (size_t r = 0; r < arguments.run_no; ++r) {
                for (size_t k = 0; k < arguments.thread_no; ++k) {
                    vthread[k] = std::thread([&]() {
                        average_ratio_promile
                            += static_cast<size_t>(single_run(arguments)*1000);
                    });
                }
                for (auto &t : vthread) {
                    t.join();
                }
            }
        }
        double average_ratio =
            static_cast<double>(average_ratio_promile)
                /1000. /* promile to fraction */
                /arguments.run_no
                /arguments.thread_no;
        end = std::chrono::system_clock::now();
        std::chrono::duration<double> duration = end-start;
        auto millis_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                .count();

        auto time_per_op =
            ((double)millis_elapsed) / arguments.iter_no;

        RunRet ret(
            time_per_op / (arguments.run_no * arguments.thread_no),
            average_ratio);
        ret.singleRatios = single_ratios;

        return ret;
    }

    virtual ~counter_bench_alloc() = default;

protected:
    static constexpr size_t M_SIZES_SIZE=5;
    const size_t m_sizes[M_SIZES_SIZE] = { 20, 40, 80, 25, 50};
//     const size_t m_sizes[M_SIZES_SIZE] = { 20, 40, 80, 25, 50, 100, 90, 70, 30, 28};
//     const size_t m_sizes[M_SIZES_SIZE] = { 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    virtual void *bench_alloc(size_t) const = 0;
    virtual void bench_free(void *) const = 0;

private:
    /// @return average hot to total ratio
    double single_run(BenchArgs& arguments) const
    {
        std::vector<void *> v;
        v.reserve(arguments.iter_no);
        for (size_t i = 0; i < arguments.iter_no; i++) {
            v.emplace_back(arguments.test_tiering ?
                bench_alloc_touch(m_sizes[i%M_SIZES_SIZE], (i%5)*(i%5)*(i%5), i)
                : bench_alloc(m_sizes[i%M_SIZES_SIZE]));
        }
        double ratio = memtier_kind_get_actual_hot_to_total_allocated_ratio();
        for (size_t i = 0; i < arguments.iter_no; i++) {
            bench_free(v[i]);
        }
        v.clear();

        return ratio;
    }

    void *bench_alloc_touch(size_t size, size_t touches, size_t step) const
    {
        void *ptr = bench_alloc(size);
        const long long one_second = 1000000000;
        for (size_t i=0; i<touches; ++i) {
            EventEntry_t entry;
            entry.type = EVENT_TOUCH;
            entry.data.touchData.address = ptr;
            entry.data.touchData.timestamp = (step*touches+i)*one_second/100;
            (void)tachanka_ranking_event_push(&entry);
        }
        return ptr;
    }
};

class memkind_bench_alloc: public counter_bench_alloc
{
protected:
    void *bench_alloc(size_t size) const final
    {
        return memkind_malloc(MEMKIND_DEFAULT, size);
    }

    void bench_free(void *ptr) const final
    {
        memkind_free(MEMKIND_DEFAULT, ptr);
    }
};

class memtier_kind_bench_alloc: public counter_bench_alloc
{
protected:
    void *bench_alloc(size_t size) const final
    {
        return memtier_kind_malloc(MEMKIND_DEFAULT, size);
    }

    void bench_free(void *ptr) const final
    {
        memtier_kind_free(MEMKIND_DEFAULT, ptr);
    }
};

class memtier_bench_alloc: public counter_bench_alloc
{
public:
    memtier_bench_alloc()
    {
        m_tier_builder = memtier_builder_new(MEMTIER_POLICY_STATIC_RATIO);
        memtier_builder_add_tier(m_tier_builder, MEMKIND_DEFAULT, 1);
        m_tier_memory =
            memtier_builder_construct_memtier_memory(m_tier_builder);
    }

    ~memtier_bench_alloc()
    {
        memtier_builder_delete(m_tier_builder);
        memtier_delete_memtier_memory(m_tier_memory);
    }

protected:
    void *bench_alloc(size_t size) const final
    {
        return memtier_malloc(m_tier_memory, size);
    }

    void bench_free(void *ptr) const final
    {
        memtier_realloc(m_tier_memory, ptr, 0);
    }

private:
    struct memtier_builder *m_tier_builder;
    struct memtier_memory *m_tier_memory;
};

class memtier_multiple_bench_alloc: public counter_bench_alloc
{
public:
    memtier_multiple_bench_alloc(memtier_policy_t policy, char *arg)
    {
        int pmem_dram_ratio = 1;
        if (arg) {
            std::cout << "Nonzero arg: " << arg << std::endl;
            pmem_dram_ratio = atoi(arg);
        }
        std::cout << "Pmem dram: " << pmem_dram_ratio << std::endl;
        assert(pmem_dram_ratio > 0);

        m_tier_builder = memtier_builder_new(policy);
        memtier_builder_add_tier(m_tier_builder, MEMKIND_DEFAULT, 1);
        memtier_builder_add_tier(m_tier_builder, MEMKIND_REGULAR, pmem_dram_ratio);
        m_tier_memory =
            memtier_builder_construct_memtier_memory(m_tier_builder);
    }

    ~memtier_multiple_bench_alloc()
    {
        memtier_builder_delete(m_tier_builder);
        memtier_delete_memtier_memory(m_tier_memory);
    }

protected:
    void *bench_alloc(size_t size) const final
    {
        return memtier_malloc(m_tier_memory, size);
    }

    void bench_free(void *ptr) const final
    {
        memtier_realloc(m_tier_memory, ptr, 0);
    }

private:
    struct memtier_builder *m_tier_builder;
    struct memtier_memory *m_tier_memory;
};

// clang-format off
static int parse_opt(int key, char *arg, struct argp_state *state)
{
    auto args = (BenchArgs *)state->input;
    switch (key) {
        case 'm':
            args->bench = Benchptr(new memkind_bench_alloc());
            break;
        case 'k':
            args->bench = Benchptr(new memtier_kind_bench_alloc());
            break;
        case 'x':
            args->bench = Benchptr(new memtier_bench_alloc());
            break;
        case 's':
            args->bench =
                Benchptr(new memtier_multiple_bench_alloc(
                    MEMTIER_POLICY_STATIC_RATIO, arg));
            break;
        case 'd':
            args->bench =
                Benchptr(new memtier_multiple_bench_alloc(
                    MEMTIER_POLICY_DYNAMIC_THRESHOLD, arg));
            break;
        case 'p':
            args->bench =
                Benchptr(new memtier_multiple_bench_alloc(
                    MEMTIER_POLICY_DATA_HOTNESS, arg));
            break;
        case 't':
            args->thread_no = std::strtol(arg, nullptr, 10);
            break;
        case 'r':
            args->run_no = std::strtol(arg, nullptr, 10);
            break;
        case 'i':
            args->iter_no = std::strtol(arg, nullptr, 10);
            break;
        case 'g':
            args->test_tiering = true;
            break;
    }
    return 0;
}

// Interface for data generation
class DataGenerator {
public:
    virtual char Generate() = 0;
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

    char Generate() {
        size_t nidx = (idx+1)%data.size();
        return data[nidx];
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

#include <memory>

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
        cumulatedTouchCoeff += touchFrequency;
        while (cumulatedTouchCoeff>1) {
            // touch - very important stuff
            EventEntry_t entry;
            entry.type = EVENT_TOUCH;
            entry.data.touchData.address = data;
            entry.data.touchData.timestamp = g_timestamp.GetPebsTimestamp();
            (void)tachanka_ranking_event_push(&entry);
            --cumulatedTouchCoeff;
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
        if (data)
            memtier_free(data);
        data = static_cast<char*>(memtier_malloc(memory.get(), size));
        memset(data, 0, size);
    }

    void GenerateSet(DataGenerator &gen) {
        assert(data && "data not initialized!");
        for (size_t i=0; i < size; ++i)
            data[i] = gen.Generate();
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
};

class AllocationTypeFactory {
    size_t maxSize, prob_coeff; // TODO upgrade prob_coeff
    std::shared_ptr<memtier_memory> memory; // TODO initialize memory
public:
    AllocationTypeFactory(
        size_t maxSize, memtier_policy_t policy, size_t pmem_dram_ratio) :
            maxSize(maxSize) {
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
        zipf_distribution<> zipf_size(maxSize);
        double max_prob=20;
        zipf_distribution<> zipf_prob((size_t)max_prob); // round max_prob down
        size_t size = zipf_size(gen);
        double probability = zipf_prob(gen)/max_prob;

        return AllocationType(size, probability, 0.1, memory);
    }
};

class AccessType {
    double getPercentage;
    std::random_device rd;
    std::shared_ptr<std::mt19937> gen;// TODO fixme
    std::shared_ptr<std::uniform_real_distribution<double>> dist;
public:
    enum class Type {
        GET,
        SET
    };
    AccessType(double get_percentage) : getPercentage(get_percentage) {
        gen = std::make_shared<std::mt19937>(rd());
        dist = std::make_shared<std::uniform_real_distribution<double>>(0, 1);
    }
    Type Generate() {
        double val = (*dist)(*gen);
        return val > getPercentage ? Type::SET : Type::GET;
    }
};

#define CHECK_TEST 1

struct RunInfo {
    size_t nTypes;
};

struct ExecResults {
    uint64_t ticks;
    uint64_t millis;
};

/// @return duration of allocations and accesses in milliseconds
ExecResults run_test(AllocationTypeFactory &factory, const RunInfo &info) {
    // TODO handle somehow max number of types
    std::vector<AllocationType> types;
    types.reserve(info.nTypes);
    for (size_t i=0; i<info.nTypes; ++i) {
        types.push_back(factory.CreateType());
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
    size_t gets=0, sets=0;
    for (int i=0; i<ITERATIONS; ++i) {
        switch (accessType.Generate()) {
            case AccessType::Type::GET:
                ++gets;
                break;
            case AccessType::Type::SET:
                ++sets;
                break;
        }
    }
    std::cout
        << "gets/sets|total: " << gets << "/" << sets << "|" << ITERATIONS
        << std::endl;
#endif
    // all data printed

    // try generating accesses
    // TODO when reallocs?????
    size_t TEST_ITERATIONS = 3000;
    size_t PER_TYPE_ITERATIONS = 100;
//     size_t TYPES = 3000;
    size_t AUX_BUFFER_SIZE=1024;
    SummatorSink sink;
    RandomInitializedGenerator gen(AUX_BUFFER_SIZE);

    auto start = std::chrono::system_clock::now();
    auto start_ticks = clock();

    for (size_t i=0; i<TEST_ITERATIONS; ++i) {
        // reallocate all types
        for (auto &type : types)
            type.Reallocate();
        // touch all types TODO ideally, they should be touched at random...
        for (size_t j = 0; j < PER_TYPE_ITERATIONS; ++j) {
            g_timestamp.AdvanceBy(15.0/PER_TYPE_ITERATIONS);
            for (auto &type : types) {
                // TODO generate access
                switch (accessType.Generate()) {
                    case AccessType::Type::GET:
                        type.GenerateGet(sink);
                        break;
                    case AccessType::Type::SET:
                        type.GenerateSet(gen);
                        break;
                }
            }
            // TODO realloc
        }
    }
    auto end = std::chrono::system_clock::now();
    auto timespan_ticks = clock() - start_ticks;

    using namespace std::chrono;
    uint64_t execution_time_millis =
        duration_cast<milliseconds>(end-start).count();

    ExecResults ret;
    ret.millis = execution_time_millis;
    ret.ticks = timespan_ticks;

    return ret;
}

int main(int argc, char *argv[])
{

    // avoid interactions between manual touches and hardware touches
    pebs_set_process_hardware_touches(false);
    RunInfo info;

    // hardcoded test constants
    info.nTypes = 5000;

    assert(argc == 2 &&
        "Incorrect number of arguments specified, "
        "please specify 1 argument: [static|hotness]"); // FIXME temporary

    // FIXME temporary arg parsing...
    memtier_policy_t policy = argv[1] == std::string("static") ?
        MEMTIER_POLICY_STATIC_RATIO
        : argv[1] == std::string("hotness") ?
            MEMTIER_POLICY_DATA_HOTNESS
            : MEMTIER_POLICY_MAX_VALUE;

    assert(policy != MEMTIER_POLICY_MAX_VALUE &&
        "please specify a known policy [hotness|static]");

    AllocationTypeFactory factory(1024, policy, 8);

    ExecResults stats = run_test(factory, info);

    std::cout
        << "Measured execution time [ticks|millis]: ["
        << stats.ticks << "|" << stats.millis << "]" << std::endl;

    //     struct BenchArgs arguments = {
//         .bench = nullptr,
//         .thread_no = 0,
//         .run_no = 1,
//         .iter_no = 10000000 };
//
//     argp_parse(&argp, argc, argv, 0, 0, &arguments);
//     RunRet run_ret =
//         arguments.bench->run(arguments);
//     double time_per_op = run_ret.averageTime;
//     double actual_ratio = run_ret.averageDramToTotalRatio;
//     std::cout << "Mean milliseconds per operation:" << time_per_op << std::endl;
//
//     for (auto &d : run_ret.singleRatios) {
//         FIXME the ratio history is weird and incorrect!!!!
//         there MUST be an error in how the allocated memory is tracked
//         std::cout
//             << "Single run ratio:  "
//             << std::fixed << std::setprecision(6) << d << std::endl;
//     }
//     std::cout
//         << "actual|desired DRAM/TOTAL ratio: "
//         << std::fixed << std::setprecision(6) << actual_ratio
//         << " | "
//         << memtier_kind_get_actual_hot_to_total_desired_ratio()
//         << std::endl;

    return 0;
}

