extern "C" {
#include "memkind/internal/ranking.h"
#include "memkind/internal/wre_avl_tree.h"
}

#include <algorithm>
#include <cassert>
#include <limits>
#include <mutex>
#include <vector>
#include <jemalloc/jemalloc.h>
#include <cstring>

#include "memkind/internal/memkind_private.h"

#define HOTNESS_MEASURE_WINDOW 1000000000ULL

// approach:
//  1) STL and inefficient data structures
//  2) tests
//  3) optimisation: AVL trees with additional cached data in nodes

// hotness: hotness (entry)

#define hotness(ttype) ((ttype)->f)

using namespace std;

struct ranking {
    double hotThreshold;
    wre_tree_t *entries;
    std::mutex mutex;
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

static void ranking_create_internal(ranking_t **ranking);
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
static void ranking_update_internal(ranking_t *ranking, struct ttype *entry_to_update, const struct ttype *updated_values);
static void touch_entry(struct ttype *entry, uint64_t timestamp, uint64_t add_hotness);

//--------private function implementation---------

void touch_entry(struct ttype *entry, uint64_t timestamp, uint64_t add_hotness)
{
    hotness (entry) += add_hotness;
    entry->t0 = timestamp;
    if(entry->timestamp_state == TIMESTAMP_NOT_SET) {
        entry->t2 = timestamp;
        entry->timestamp_state = TIMESTAMP_INIT;
    }
    if (entry->timestamp_state == TIMESTAMP_INIT_DONE) {
        if ((entry->t0 - entry->t1) > HOTNESS_MEASURE_WINDOW) {
            // move to next measurement window
            float f2 = (float)hotness (entry) * entry->t2 / (entry->t2 - entry->t0);
            float f1 = (float)entry->n1 * entry->t1 / (entry->t2 - entry->t0);
            entry->f = f2 * 0.3 + f1 * 0.7; // TODO weighted sum or sth else?
            entry->t2 = entry->t1;
            entry->t1 = entry->t0;
            hotness (entry) = entry->n1;
            entry->n1 = 0;
        }
    } else {
        // TODO init not done
        if ((entry->t0 - entry->t2) > HOTNESS_MEASURE_WINDOW) {
            // TODO - classify hotness
            entry->timestamp_state = TIMESTAMP_INIT_DONE;;
            entry->t1 = entry->t0;
        }
    }

}

void ranking_create_internal(ranking_t **ranking)
{
    *ranking = (ranking_t *)jemk_malloc(sizeof(ranking_t));
    wre_create(&(*ranking)->entries, is_hotter_agg_hot);
    // placement new for mutex
    // mutex is already inside a structure, so alignment should be ok
    (void)new ((void*)(&(*ranking)->mutex)) std::mutex();
    (*ranking)->hotThreshold=0.;
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

    return ranking_get_hot_threshold_internal(ranking);
}

double
ranking_calculate_hot_threshold_dram_pmem_internal(ranking_t *ranking,
                                                   double dram_pmem_ratio)
{
    double ratio = dram_pmem_ratio / (1 + dram_pmem_ratio);
    return ranking_calculate_hot_threshold_dram_total_internal(ranking, ratio);
}

void ranking_add_internal(ranking_t *ranking, struct ttype *entry)
{
    AggregatedHotness temp;
    temp.hotness = hotness (entry); // only hotness matters for lookup
    AggregatedHotness_t *value =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
    if (value) {
        // value with the same hotness is already there, should be aggregated
        value->size += entry->size;
    } else {
        value = (AggregatedHotness_t *)jemk_malloc(sizeof(AggregatedHotness_t));
        value->hotness = hotness (entry);
        value->size = entry->size;
    }
    wre_put(ranking->entries, value, value->size);
}

bool ranking_is_hot_internal(ranking_t *ranking, struct ttype *entry)
{
    return hotness (entry) >= ranking_get_hot_threshold_internal(ranking);
}

void ranking_remove_internal(ranking_t *ranking, const struct ttype *entry)
{
    AggregatedHotness temp;
    temp.hotness = hotness (entry); // only hotness matters for lookup
    AggregatedHotness_t *removed =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
    if (removed) {
        assert(entry->size <= removed->size);
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

static void ranking_update_internal(ranking_t *ranking, struct ttype *entry_to_update, const struct ttype *updated_values)
{
    ranking_remove_internal(ranking, entry_to_update);
    memcpy(entry_to_update, updated_values, sizeof(*entry_to_update));
    ranking_add_internal(ranking, entry_to_update);
}

void ranking_touch_internal(ranking_t *ranking, struct ttype *entry, uint64_t timestamp, uint64_t add_hotness)
{
    ranking_remove_internal(ranking, entry);
    touch_entry(entry, timestamp, add_hotness);
    ranking_add_internal(ranking, entry);
}

//--------public function implementation---------

MEMKIND_EXPORT void ranking_create(ranking_t **ranking)
{
    ranking_create_internal(ranking);
}

MEMKIND_EXPORT void ranking_destroy(ranking_t *ranking)
{
    ranking_destroy_internal(ranking);
}

MEMKIND_EXPORT double ranking_get_hot_threshold(ranking_t *ranking)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_get_hot_threshold_internal(ranking);
}

MEMKIND_EXPORT double ranking_calculate_hot_threshold_dram_total(ranking_t *ranking,
                                                  double dram_pmem_ratio)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_calculate_hot_threshold_dram_total_internal(ranking,
                                                               dram_pmem_ratio);
}

MEMKIND_EXPORT double ranking_calculate_hot_threshold_dram_pmem(ranking_t *ranking,
                                                 double dram_pmem_ratio)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_calculate_hot_threshold_dram_pmem_internal(ranking,
                                                              dram_pmem_ratio);
}

MEMKIND_EXPORT void ranking_add(ranking_t *ranking, struct ttype *entry)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    ranking_add_internal(ranking, entry);
}

MEMKIND_EXPORT bool ranking_is_hot(ranking_t *ranking, struct ttype *entry)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_is_hot_internal(ranking, entry);
}

MEMKIND_EXPORT void ranking_remove(ranking_t *ranking, const struct ttype *entry)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    ranking_remove_internal(ranking, entry);
}

MEMKIND_EXPORT void ranking_update(ranking_t *ranking, struct ttype *entry_to_update, const struct ttype *updated_value)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    ranking_update_internal(ranking, entry_to_update, updated_value);
}

MEMKIND_EXPORT void ranking_touch(ranking_t *ranking, struct ttype *entry, uint64_t timestamp, uint64_t add_hotness)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    ranking_touch_internal(ranking, entry, timestamp, add_hotness);
}
