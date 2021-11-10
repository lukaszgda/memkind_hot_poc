#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ranking_info {
    double integrated_error;
    double proportional;
    double integral;
//     double derivative; // This would require filtering!!!
    double hotTierSize;
    double coldTierSize;
} ranking_controller;

/// @p gain 1) has no effect (0;1) lowers response, (1;inf) amplifies response
extern void ranking_controller_init_ranking_controller(
    ranking_controller *controller, double expected_dram_total,
    double proportional_term, double integral_term);

extern void
ranking_controller_set_expected_dram_total(ranking_controller *controller,
                                          double expected_dram_total);

extern double ranking_controller_calculate_fixed_thresh(
    ranking_controller *controller, double found_dram_total);

#ifdef __cplusplus
}
#endif
