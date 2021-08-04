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

using namespace std;

struct ranking {
    std::vector<ranking_entry_t> entries; /// ordered hottest -> coldest
};

// IF to implement:
// TODO mapping ranking_entry -> RankingEntry

//--------private function implementation---------
static bool is_hotter(ranking_entry_t a, ranking_entry_t b) {
    return a.hotness > b.hotness;
}

//--------public function implementation---------

void ranking_create(ranking_t **ranking) {
    *ranking = new struct ranking();
}

void ranking_destroy(ranking_t *ranking) {
    delete ranking;
}

/// @pre ranking should be sorted
double ranking_get_hot_threshold(ranking_t *ranking, double dram_pmem_ratio) {
    // O(N) -> needs to be upgraded to O(log(N)) by using a tree
    if (ranking->entries.size() == 0)
        return std::numeric_limits<double>::min();

    size_t total_size=0;
    for (auto &entry : ranking->entries) {
        total_size += entry.size;
    }
    size_t threshold_size=dram_pmem_ratio*total_size;
    total_size=0;
    for (auto &entry : ranking->entries) {
        total_size += entry.size;
        if (total_size >= threshold_size) {
            return entry.hotness;
        }
    }
    return ranking->entries.back().hotness;
}

void ranking_add(ranking_t *ranking, ranking_entry_t *entry) {
    // O(N) -> needs to be upgraded to O(log(N)) by using a tree
    ranking->entries.push_back(*entry);
    sort(ranking->entries.begin(), ranking->entries.end(), is_hotter);
}

void ranking_remove(ranking_t *ranking, const ranking_entry_t *entry) {
    // TODO
    auto &t = ranking->entries;
    t.erase(remove(t.begin(), t.end(), *entry), t.end());
//     auto find(ranking->entries.begin(), ranking->entries.end(), *entry);
}
