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

// approach:
//  1) STL and inefficient data structures
//  2) tests
//  3) optimisation: AVL trees with additional cached data in nodes

// hotness: entry->n2

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

//--------private function implementation---------

void ranking_create_internal(ranking_t **ranking)
{
    *ranking = (ranking_t *)jemk_malloc(sizeof(ranking_t));
    wre_create(&(*ranking)->entries, is_hotter_agg_hot);
}

void ranking_destroy_internal(ranking_t *ranking)
{
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
    temp.hotness = entry->n2; // only hotness matters for lookup
    AggregatedHotness_t *value =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
    if (value) {
        // value with the same hotness is already there, should be aggregated
        value->size += entry->size;
    } else {
        value = (AggregatedHotness_t *)jemk_malloc(sizeof(AggregatedHotness_t));
        value->hotness = entry->n2;
        value->size = entry->size;
    }
    wre_put(ranking->entries, value, value->size);
}

bool ranking_is_hot_internal(ranking_t *ranking, struct ttype *entry)
{
    return entry->n2 >= ranking_get_hot_threshold_internal(ranking);
}

void ranking_remove_internal(ranking_t *ranking, const struct ttype *entry)
{
    AggregatedHotness temp;
    temp.hotness = entry->n2; // only hotness matters for lookup
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

//--------public function implementation---------

void ranking_create(ranking_t **ranking)
{
    ranking_create_internal(ranking);
}

void ranking_destroy(ranking_t *ranking)
{
    ranking_destroy_internal(ranking);
}

double ranking_get_hot_threshold(ranking_t *ranking)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_get_hot_threshold_internal(ranking);
}

double ranking_calculate_hot_threshold_dram_total(ranking_t *ranking,
                                                  double dram_pmem_ratio)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_calculate_hot_threshold_dram_total_internal(ranking,
                                                               dram_pmem_ratio);
}

double ranking_calculate_hot_threshold_dram_pmem(ranking_t *ranking,
                                                 double dram_pmem_ratio)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_calculate_hot_threshold_dram_pmem_internal(ranking,
                                                              dram_pmem_ratio);
}

void ranking_add(ranking_t *ranking, struct ttype *entry)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    ranking_add_internal(ranking, entry);
    ;
}

bool ranking_is_hot(ranking_t *ranking, struct ttype *entry)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    return ranking_is_hot_internal(ranking, entry);
}

void ranking_remove(ranking_t *ranking, const struct ttype *entry)
{
    std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    ranking_remove_internal(ranking, entry);
}
