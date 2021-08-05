extern "C" {
#include "memkind/internal/ranking.h"
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
    std::vector<struct tblock*> entries; /// ordered hottest -> coldest
};

// IF to implement:
// TODO mapping ranking_entry -> RankingEntry

//--------private function implementation---------
static bool is_hotter(struct tblock *a, struct tblock *b) {
    return a->n2 > b->n2;
}

//--------public function implementation---------

void ranking_create(ranking_t **ranking) {
    *ranking = new struct ranking();
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

    // O(N) -> needs to be upgraded to O(log(N)) by using a tree
    if (ranking->entries.size() == 0)
        return std::numeric_limits<double>::min();

    size_t total_size=0;
    for (auto &entry : ranking->entries) {
        total_size += entry->size;
    }
    size_t threshold_size=dram_pmem_ratio*total_size;
    total_size=0;
    ranking->hotThreshold=ranking->entries.back()->n2;
    for (auto &entry : ranking->entries) {
        total_size += entry->size;
        if (total_size >= threshold_size) {
            ranking->hotThreshold=entry->n2;
            break;;
        }
    }
    return ranking_get_hot_threshold(ranking);
}

void ranking_add(ranking_t *ranking, struct tblock *entry) {
    // O(N) -> needs to be upgraded to O(log(N)) by using a tree
    ranking->entries.push_back(entry);
    sort(ranking->entries.begin(), ranking->entries.end(), is_hotter);
}

bool ranking_is_hot(ranking_t *ranking, struct tblock *entry) {
    return entry->n2 >= ranking_get_hot_threshold(ranking);
}

void ranking_remove(ranking_t *ranking, const struct tblock *entry) {
    auto &t = ranking->entries;
    t.erase(remove(t.begin(), t.end(), entry), t.end());
}
