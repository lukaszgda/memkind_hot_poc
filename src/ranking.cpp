extern "C" {
#include "memkind/internal/ranking.h"
#include "memkind/internal/wre_avl_tree.h"
}

#include <vector>
#include <limits>
#include <algorithm>
#include <cassert>

// approach:
//  1) STL and inefficient data structures
//  2) tests
//  3) optimisation: AVL trees with additional cached data in nodes

// hotness: entry->n2

using namespace std;

struct ranking {
    double hotThreshold;
    wre_tree_t *entries;
};

typedef struct AggregatedHotness {
    size_t size;
    double hotness;
} AggregatedHotness_t;

//--------private function implementation---------

static bool is_hotter_agg_hot(const void *a, const void *b) {
    double a_hot = ((AggregatedHotness_t*)a)->hotness;
    double b_hot = ((AggregatedHotness_t*)b)->hotness;
    return a_hot > b_hot;
}

//--------public function implementation---------

void ranking_create(ranking_t **ranking) {
    *ranking = new struct ranking();
    wre_create(&(*ranking)->entries, is_hotter_agg_hot);
}

void ranking_destroy(ranking_t *ranking) {
    delete ranking;
}

double ranking_get_hot_threshold(ranking_t *ranking) {
    return ranking->hotThreshold;
}

/// @pre ranking should be sorted
double ranking_calculate_hot_threshold(
    ranking_t *ranking, double dram_pmem_ratio) {

    ranking->hotThreshold=0;
    AggregatedHotness_t *agg_hot = (AggregatedHotness_t*)
        wre_find_weighted(ranking->entries, dram_pmem_ratio);
    if (agg_hot) {
        ranking->hotThreshold=agg_hot->hotness;
    }

    return ranking_get_hot_threshold(ranking);
}

void ranking_add(ranking_t *ranking, struct tblock *entry) {
    AggregatedHotness temp;
    temp.hotness=entry->n2; // only hotness matters for lookup
    AggregatedHotness_t* value =
        (AggregatedHotness_t*)wre_remove(ranking->entries, &temp);
    if (value) {
        // value with the same hotness is already there, should be aggregated
        value->size += entry->size;
    } else {
        value = (AggregatedHotness_t*)malloc(sizeof(AggregatedHotness_t));
        value->hotness = entry->n2;
        value->size = entry->size;
    }
    wre_put(ranking->entries, value, value->size);
}

bool ranking_is_hot(ranking_t *ranking, struct tblock *entry) {
    return entry->n2 >= ranking_get_hot_threshold(ranking);
}

void ranking_remove(ranking_t *ranking, const struct tblock *entry) {
    AggregatedHotness temp;
    temp.hotness=entry->n2; // only hotness matters for lookup
    AggregatedHotness_t *removed =
        (AggregatedHotness_t*)wre_remove(ranking->entries, &temp);
    if (removed) {
        assert(entry->size <= removed->size);
        removed->size -= entry->size;
        if (removed->size == 0)
            free(removed);
        else
            wre_put(ranking->entries, removed, removed->size);
    } else {
        // TODO add error handling instead of assert
        assert(false); // entry does not exist, error occurred
    }
}
