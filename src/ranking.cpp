extern "C" {
#include "memkind/internal/ranking.h"
#include "memkind/internal/wre_avl_tree.h"
}

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <jemalloc/jemalloc.h>
#include <limits>
#include <mutex>
#include <vector>

#include "memkind/internal/memkind_private.h"
#include "memkind/internal/memkind_log.h"
#include "memkind/internal/memkind_memtier.h"

#define THREAD_SAFE

#ifdef THREAD_SAFE
#define RANKING_LOCK_GUARD(ranking) std::lock_guard<std::mutex> lock_guard((ranking)->mutex)
#else
#define RANKING_LOCK_GUARD(ranking) (void)(ranking)->mutex
#endif

// approach:
//  1) STL and inefficient data structures
//  2) tests
//  3) optimisation: AVL trees with additional cached data in nodes

using namespace std;

struct ranking {
    std::atomic<double> hotThreshold;
    wre_tree_t *entries;
    std::mutex mutex;
    double oldWeight;
    double newWeight;
};

typedef struct AggregatedHotness {
    size_t size;
    double hotness;
} AggregatedHotness_t;

//--------private function implementation---------

static bool is_hotter_agg_hot(const void *a, const void *b)
{
    double a_hot = ((AggregatedHotness_t *)a)->hotness;
    double b_hot = ((AggregatedHotness_t *)b)->hotness;
    return a_hot > b_hot;
}

static void ranking_create_internal(ranking_t **ranking, double old_weight);
static void ranking_destroy_internal(ranking_t *ranking);
static void ranking_add_internal(ranking_t *ranking, struct ttype *entry);
static void ranking_remove_internal(ranking_t *ranking,
                                    const struct ttype *entry);
static double ranking_get_hot_threshold_internal(ranking_t *ranking);
static double
ranking_calculate_hot_threshold_dram_total_internal(ranking_t *ranking,
                                                    double dram_pmem_ratio);
static double
ranking_calculate_hot_threshold_dram_pmem_internal(ranking_t *ranking,
                                                   double dram_pmem_ratio);
static bool ranking_is_hot_internal(ranking_t *ranking, struct ttype *entry);
static void ranking_update_internal(ranking_t *ranking,
                                    struct ttype *entry_to_update,
                                    const struct ttype *updated_values);
static void ranking_touch_entry_internal(ranking_t *ranking,
                                         struct ttype *entry,
                                         uint64_t timestamp,
                                         uint64_t add_hotness);

//--------private function implementation---------

#if 1
// old touch entry definition - as described in design doc
void ranking_touch_entry_internal(ranking_t *ranking, struct ttype *entry,
                                  uint64_t timestamp, uint64_t add_hotness)
{
    //     printf("touches internal internal, timestamp: [%lu]\n", timestamp);

    if (entry->touchCb)
        entry->touchCb(entry->touchCbArg);

    entry->n1 += add_hotness;
    entry->t0 = timestamp;
    if (timestamp != 0) {
        if (entry->timestamp_state == TIMESTAMP_NOT_SET) {
            entry->t2 = timestamp;
            entry->timestamp_state = TIMESTAMP_INIT;
        }

        if (entry->timestamp_state == TIMESTAMP_INIT_DONE) {
            if ((entry->t0 - entry->t1) > HOTNESS_MEASURE_WINDOW) {
                // move to next measurement window
                float f2 = ((float)entry->n2) / (entry->t1 - entry->t2);
                float f1 = ((float)entry->n1) / (entry->t0 - entry->t1);
                entry->f = f2 * ranking->oldWeight + f1 * ranking->newWeight;
                entry->t2 = entry->t1;
                entry->t1 = entry->t0;
                // TODO - n2 should be calclated differently
                entry->n2 = entry->n1;
                entry->n1 = 0;
                //                 printf("wre: hotness updated: [new, old]:
                //                 [%.16f, %.16f]\n", f1, f2);
            }
            //             printf("wre: hotness awaiting window\n");
        } else {
            // TODO init not done
            //             printf("wre: hotness awaiting window\n");
            if ((entry->t0 - entry->t2) > HOTNESS_MEASURE_WINDOW) {
                // TODO - classify hotness
                entry->timestamp_state = TIMESTAMP_INIT_DONE;
                ;
                entry->t1 = entry->t0;
                //                 printf("wre: hotness init done\n");
            }
        }
    } else {
        //         printf("wre: hotness touch without timestamp!\n");
    }
}

#else

void touch_entry(struct ttype *entry, uint64_t timestamp, uint64_t add_hotness)
{
    if (entry->touchCb)
        entry->touchCb(entry->touchCbArg);

    hotness(entry) += add_hotness;
}

#endif

void ranking_create_internal(ranking_t **ranking, double old_weight)
{
    *ranking = (ranking_t *)jemk_malloc(sizeof(ranking_t));
    wre_create(&(*ranking)->entries, is_hotter_agg_hot);
    // placement new for mutex
    // mutex is already inside a structure, so alignment should be ok
    (void)new ((void *)(&(*ranking)->mutex)) std::mutex();
    (*ranking)->hotThreshold = 0.;
    (*ranking)->oldWeight = old_weight;
    (*ranking)->newWeight = 1 - old_weight;
}

void ranking_destroy_internal(ranking_t *ranking)
{
    // explicit destructor call for mutex
    // which was created with placement new
    ranking->mutex.~mutex();
    jemk_free(ranking);
}

double ranking_get_hot_threshold_internal(ranking_t *ranking)
{
    return ranking->hotThreshold;
}

double
ranking_calculate_hot_threshold_dram_total_internal(ranking_t *ranking,
                                                    double dram_pmem_ratio)
{

    ranking->hotThreshold = 0;
    AggregatedHotness_t *agg_hot = (AggregatedHotness_t *)wre_find_weighted(
        ranking->entries, dram_pmem_ratio);
    if (agg_hot) {
        ranking->hotThreshold = agg_hot->hotness;
    }
    // TODO remove this!!!
    //     printf("wre: threshold_dram_total_internal\n");
    // EOF TODO

    return ranking_get_hot_threshold_internal(ranking);
}

double
ranking_calculate_hot_threshold_dram_pmem_internal(ranking_t *ranking,
                                                   double dram_pmem_ratio)
{
    // TODO remove this!!!
    //     printf("wre: threshold_dram_pmem_internal\n");
    // EOF TODO

    double ratio = dram_pmem_ratio / (1 + dram_pmem_ratio);
    return ranking_calculate_hot_threshold_dram_total_internal(ranking, ratio);
}

void ranking_add_internal(ranking_t *ranking, struct ttype *entry)
{
    AggregatedHotness temp;
    temp.hotness = entry->f; // only hotness matters for lookup // TODO: rrudnick ????
    AggregatedHotness_t *value =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
    if (value) {
        // value with the same hotness is already there, should be aggregated
        value->size += entry->size;
        //         printf("wre: hotness found, aggregates\n");
    } else {
        value = (AggregatedHotness_t *)jemk_malloc(sizeof(AggregatedHotness_t));
        value->hotness = entry->f;
        value->size = entry->size;
        //         printf("wre: hotness not found, adds\n");
    }
    wre_put(ranking->entries, value, value->size);
}

bool ranking_is_hot_internal(ranking_t *ranking, struct ttype *entry)
{
    return entry->f >= ranking_get_hot_threshold_internal(ranking);
}

void ranking_remove_internal(ranking_t *ranking, const struct ttype *entry)
{
    AggregatedHotness temp;
    temp.hotness = entry->f; // only hotness matters for lookup
    AggregatedHotness_t *removed =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
    if (removed) {
        if (entry->size > removed->size)
        {
            log_fatal("ranking_remove_internal: tried to removed more that added!");
        }
        removed->size -= entry->size;
        if (removed->size == 0)
            jemk_free(removed);
        else
            wre_put(ranking->entries, removed, removed->size);
    } else {
        // TODO add error handling instead of assert
        assert(false); // entry does not exist, error occurred
    }
}

static void ranking_update_internal(ranking_t *ranking,
                                    struct ttype *entry_to_update,
                                    const struct ttype *updated_values)
{
    ranking_remove_internal(ranking, entry_to_update);
    memcpy(entry_to_update, updated_values, sizeof(*entry_to_update));
    ranking_add_internal(ranking, entry_to_update);
}

void ranking_touch_internal(ranking_t *ranking, struct ttype *entry,
                            uint64_t timestamp, uint64_t add_hotness)
{
    //     printf("touches internal, timestamp: [%lu]\n", timestamp);
    ranking_remove_internal(ranking, entry);
    ranking_touch_entry_internal(ranking, entry, timestamp, add_hotness);
    ranking_add_internal(ranking, entry);
}

//--------public function implementation---------

MEMKIND_EXPORT void ranking_create(ranking_t **ranking, double old_weight)
{
    ranking_create_internal(ranking, old_weight);
}

MEMKIND_EXPORT void ranking_destroy(ranking_t *ranking)
{
    ranking_destroy_internal(ranking);
}

MEMKIND_EXPORT double ranking_get_hot_threshold(ranking_t *ranking)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    return ranking_get_hot_threshold_internal(ranking);
}

MEMKIND_EXPORT double
ranking_calculate_hot_threshold_dram_total(ranking_t *ranking,
                                           double dram_pmem_ratio)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    return ranking_calculate_hot_threshold_dram_total_internal(ranking,
                                                               dram_pmem_ratio);
}

MEMKIND_EXPORT double
ranking_calculate_hot_threshold_dram_pmem(ranking_t *ranking,
                                          double dram_pmem_ratio)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    return ranking_calculate_hot_threshold_dram_pmem_internal(ranking,
                                                              dram_pmem_ratio);
}

MEMKIND_EXPORT void ranking_add(ranking_t *ranking, struct ttype *entry)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    ranking_add_internal(ranking, entry);
}

MEMKIND_EXPORT bool ranking_is_hot(ranking_t *ranking, struct ttype *entry)
{
    // mutex not necessary
    // std::lock_guard<std::mutex> lock_guard(ranking->mutex);
//     RANKING_LOCK_GUARD(ranking);
    return ranking_is_hot_internal(ranking, entry);
}

MEMKIND_EXPORT void ranking_remove(ranking_t *ranking,
                                   const struct ttype *entry)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    ranking_remove_internal(ranking, entry);
}

MEMKIND_EXPORT void ranking_update(ranking_t *ranking,
                                   struct ttype *entry_to_update,
                                   const struct ttype *updated_value)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    ranking_update_internal(ranking, entry_to_update, updated_value);
}

MEMKIND_EXPORT void ranking_touch(ranking_t *ranking, struct ttype *entry,
                                  uint64_t timestamp, uint64_t add_hotness)
{
    //     printf("touches ranking, timestamp: [%lu]\n", timestamp);
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    ranking_touch_internal(ranking, entry, timestamp, add_hotness);
}

MEMKIND_EXPORT void ranking_set_touch_callback(ranking_t *ranking,
                                               tachanka_touch_callback cb,
                                               void *arg, struct ttype *type)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    type->touchCb = cb;
    type->touchCbArg = arg;
}
