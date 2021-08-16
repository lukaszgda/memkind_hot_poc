#pragma once
#include "stdlib.h"
#include "stdbool.h"

#include "memkind/internal/tachanka.h"

typedef struct ranking ranking_t;


// --- minimal API ---
// Basic workflow:
//
//      ranking_add     // once per entry
//      ranking_touch   // as many times as necessary per entry
//      ranking_touch   // as many times as necessary per entry
//      ranking_touch   // as many times as necessary per entry
//      ranking_touch   // as many times as necessary per entry
//      ranking_remove  // when entry is no longer used

extern void ranking_create(ranking_t **ranking);
extern void ranking_destroy(ranking_t *ranking);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
extern void ranking_add(ranking_t *ranking, struct ttype *entry);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
/// @pre @p entry should already be added to ranking
extern void ranking_touch(ranking_t *ranking, struct ttype *entry, uint64_t timestamp, uint64_t add_hotness);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
extern void ranking_remove(ranking_t *ranking, const struct ttype *entry);

// --- extended API ---
/// @brief atomically update values of @p entry_to_update with values from @p updated_values
///
/// workflow:
///     1) find and remove @p entry_to_update from ranking
///     2) memcpy updated_value to entry_to_update
///     3) add entry to update to ranking
extern void ranking_update(ranking_t *ranking, struct ttype *entry_to_update, const struct ttype *updated_value);
/// get last calculated hot threshold
extern double ranking_get_hot_threshold(ranking_t *ranking);
/// @p dram_pmem_ratio : dram/(dram+pmem)
extern double
ranking_calculate_hot_threshold_dram_total(ranking_t *ranking,
                                           double dram_pmem_ratio);
/// @p dram_pmem_ratio : dram/pmem (does not support 0 pmem)
extern double ranking_calculate_hot_threshold_dram_pmem(ranking_t *ranking,
                                                        double dram_pmem_ratio);
extern bool ranking_is_hot(ranking_t *ranking, struct ttype *entry);


extern void ranking_set_monitoring(ranking_t *ranking, const char* name, struct ttype *type);
