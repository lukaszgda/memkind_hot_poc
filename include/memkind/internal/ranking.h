#pragma once
#include "stdlib.h"

typedef struct ranking ranking_t;

// TODO use struct tblock later in the future/ incorporate it to ranking entry
typedef struct ranking_entry {
    double hotness;
    size_t size;
} ranking_entry_t;

extern void ranking_create(ranking_t **ranking);
extern void ranking_destroy(ranking_t *ranking);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
extern void ranking_add(ranking_t *ranking, ranking_entry_t *entry);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
extern void ranking_remove(ranking_t *ranking, const ranking_entry_t *entry);
/// get last calculated hot threshold
extern double ranking_get_hot_threshold(ranking_t *ranking);
extern double ranking_calculate_hot_threshold(
    ranking_t *ranking, double dram_pmem_ratio);
extern bool ranking_is_hot(ranking_t *ranking, ranking_entry_t *entry);
