#pragma once
#include "stdlib.h"

#include "memkind/internal/tachanka.h"

typedef struct ranking ranking_t;

extern void ranking_create(ranking_t **ranking);
extern void ranking_destroy(ranking_t *ranking);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
extern void ranking_add(ranking_t *ranking, struct tblock *entry);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
extern void ranking_remove(ranking_t *ranking, const struct tblock *entry);
/// get last calculated hot threshold
extern double ranking_get_hot_threshold(ranking_t *ranking);
/// @p dram_pmem_ratio : dram/(dram+pmem)
extern double ranking_calculate_hot_threshold_dram_total(
    ranking_t *ranking, double dram_pmem_ratio);
/// @p dram_pmem_ratio : dram/pmem (does not support 0 pmem)
extern double ranking_calculate_hot_threshold_dram_pmem(
    ranking_t *ranking, double dram_pmem_ratio);
extern bool ranking_is_hot(ranking_t *ranking, struct tblock *entry);
