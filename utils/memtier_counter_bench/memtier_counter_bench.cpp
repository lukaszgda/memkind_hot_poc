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
                bench_alloc_touch(m_sizes[i%M_SIZES_SIZE], (i%5)*(i%5)*(i%5))
                : bench_alloc(m_sizes[i%M_SIZES_SIZE]));
        }
        double ratio = memtier_kind_get_actual_hot_to_total_allocated_ratio();
        for (size_t i = 0; i < arguments.iter_no; i++) {
            bench_free(v[i]);
        }
        v.clear();

        return ratio;
    }

    void *bench_alloc_touch(size_t size, size_t touches) const
    {
        void *ptr = bench_alloc(size);
        const long long one_second = 1000000000;
        for (size_t i=0; i<touches; ++i)
            touch(ptr, i*one_second, 0);
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

static struct argp_option options[] = {
    {"memkind", 'm', 0, 0, "Benchmark memkind."},
    {"memtier_kind", 'k', 0, 0, "Benchmark memtier_memkind."},
    {"memtier", 'x', 0, 0, "Benchmark memtier_memory - single tier."},
    {"memtier_multiple", 's', "int", 0, "Benchmark memtier_memory - two tiers, static ratio."},
    {"memtier_multiple", 'd', "int", 0, "Benchmark memtier_memory - two tiers, dynamic threshold, pmem/dram ratio."},
    {"memtier_multiple", 'p', "int", 0, "Benchmark memtier_memory - two tiers, data hotness, pmem/dram ratio."},
    {"thread", 't', "int", 0, "Threads numbers."},
    {"runs", 'r', "int", 0, "Benchmark run numbers."},
    {"iterations", 'i', "int", 0, "Benchmark iteration numbers."},
    {"test_tiering", 'g', 0, 0, "Test tiering in addition to malloc overhead."},
    {0}};
// clang-format on

static struct argp argp = {options, parse_opt, nullptr, nullptr};

int main(int argc, char *argv[])
{
    struct BenchArgs arguments = {
        .bench = nullptr,
        .thread_no = 0,
        .run_no = 1,
        .iter_no = 10000000 };

    argp_parse(&argp, argc, argv, 0, 0, &arguments);
    RunRet run_ret =
        arguments.bench->run(arguments);
    double time_per_op = run_ret.averageTime;
    double actual_ratio = run_ret.averageDramToTotalRatio;
    std::cout << "Mean milliseconds per operation:" << time_per_op << std::endl;

    for (auto &d : run_ret.singleRatios) {
        // FIXME the ratio history is weird and incorrect!!!!
        // there MUST be an error in how the allocated memory is tracked
        std::cout
            << "Single run ratio:  "
            << std::fixed << std::setprecision(6) << d << std::endl;
    }
    std::cout
        << "actual|desired DRAM/TOTAL ratio: "
        << std::fixed << std::setprecision(6) << actual_ratio
        << " | "
        << memtier_kind_get_actual_hot_to_total_desired_ratio()
        << std::endl;

    return 0;
}
