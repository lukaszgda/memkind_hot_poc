extern "C" {
#include "memkind/internal/ranking.h"
#include "memkind/internal/wre_avl_tree.h"
}

#include <vector>
#include <limits>
#include <algorithm>

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

// IF to implement:
// TODO mapping ranking_entry -> RankingEntry

//--------private function implementation---------

static bool is_hotter_tblock(const void *a, const void *b) {
    return ((struct tblock*)a)->n2 > ((struct tblock*)b)->n2;
}

//--------public function implementation---------

void ranking_create(ranking_t **ranking) {
    *ranking = new struct ranking();
    wre_create(&(*ranking)->entries, is_hotter_tblock);
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

    struct tblock *block =
        (struct tblock*)wre_find_weighted(ranking->entries, dram_pmem_ratio);
    if (block) {
        ranking->hotThreshold=block->n2;
    }

    return ranking_get_hot_threshold(ranking);
}

void ranking_add(ranking_t *ranking, struct tblock *entry) {
    wre_put(ranking->entries, entry, entry->size);
}

bool ranking_is_hot(ranking_t *ranking, struct tblock *entry) {
    return entry->n2 >= ranking_get_hot_threshold(ranking);
}

void ranking_remove(ranking_t *ranking, const struct tblock *entry) {
    bool removed = wre_remove(ranking->entries, entry);
    if (!removed) {
        // element to remove not found; TODO handle error
    }
}
