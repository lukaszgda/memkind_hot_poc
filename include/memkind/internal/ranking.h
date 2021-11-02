#pragma once
#include "stdbool.h"
#include "stdlib.h"

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

/// @p old_weight weight of old time window
///
/// Hotness is calculated as a weighted sum between old hotness
/// and current frequency:
///     - @p old_weight is the weight of old hotness,
///     - (1 - @p old_weight) is the weight of current frequency
extern void ranking_create(ranking_t **ranking, double old_weight);
extern void ranking_destroy(ranking_t *ranking);
extern void ranking_add(ranking_t *ranking, double hotness, size_t size);
/// @p entry ownership stays with the caller
/// @p entry should not be freed until it is removed from ranking
/// @pre @p entry should already be added to ranking
/// @p timestamp timestamp from pebs
///
/// @warning if correct timestamp is unknown (e.g. touched from
/// malloc, not pebs), value "0" can be passed - timestamp will be ignored
extern void ranking_touch(ranking_t *ranking, struct ttype *entry,
                          uint64_t timestamp, double add_hotness);
extern void ranking_remove(ranking_t *ranking, double hotness, size_t size);

// --- extended API ---
/// @brief atomically update values of @p entry_to_update with values from @p
/// updated_values
///
/// workflow:
///     1) find and remove @p entry_to_update from ranking
///     2) memcpy updated_value to entry_to_update
///     3) add entry to update to ranking
// extern void ranking_update(ranking_t *ranking, struct ttype *entry_to_update,
//                            const struct ttype *updated_value);
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

/// @warning @p callback will be executed from the context of touch function,
/// from multiple threads, under mutex (call atomicity is guaranteed)
/// DO NOT:
///     1) PERFORM MEMORY ALLOCATIONS traceble by ranking - it might result in:
///         a) indirect recursion,
///         b) infinite loops,
///         c) out of memory errors,
///         d) other horrors,
///     2) perform cpu intensive tasks,
///     3) perform time-consuming tasks: sleeps, networking, intensive io, etc
/// PLEASE BE CAUTIOUS WHEN USING THIS FEATURE!
extern void ranking_set_touch_callback(ranking_t *ranking,
                                       tachanka_touch_callback cb, void *arg,
                                       struct ttype *type);

size_t ranking_calculate_total_size(ranking_t *ranking);
