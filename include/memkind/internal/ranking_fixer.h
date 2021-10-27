#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ranking_info {
    double gain;
    double hotTierSize;
    double coldTierSize;
} ranking_info;

/// @p gain 1) has no effect (0;1) lowers response, (1;inf) amplifies response
extern void ranking_fixer_init_ranking_info(
    ranking_info *info, double expected_dram_total, double gain);

extern double ranking_fixer_calculate_fixed_thresh(
    ranking_info *info, double found_dram_total);

#ifdef __cplusplus
}
#endif
