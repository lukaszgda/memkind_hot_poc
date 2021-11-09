#include "memkind/internal/ranking_controller.h"

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif


MEMKIND_EXPORT void ranking_controller_init_ranking_controller(
    ranking_controller *controller, double expected_dram_total, double gain) {
    controller->gain = gain;
    controller->hotTierSize = expected_dram_total;
    controller->coldTierSize = 1-expected_dram_total;
}

MEMKIND_EXPORT double ranking_controller_calculate_fixed_thresh(ranking_controller *controller, double found_dram_total) {

    // case: found thresh too low
    // |---a----------|--------b--|
    // |---c--|----------------d--|
    // |---c--|-(a-c)-|-----------|
    // |---a----------|-e-|-------|
    // (a-c)/a==e/b
    // e = b/a * (a-c)

    // case: found thresh too high
    // |---a----------|--------b--|
    // |---c------------------|-d-|
    // |---c------------------|-d-|
    // |---c----------|-(c-a)-|---|
    // |-------|--e---|-----------|
    // (c-a)/b==e/a
    // e = - a/b * (c-a)
    double a=controller->coldTierSize;
    double b=controller->hotTierSize;
    double c = 1-found_dram_total;
    // double d = 1-found_ratio; // unused
    double t=a-c;

    if (a == c && a == 0)
        // corner case - the formula below gives 0/0 (indeterminate form)
        return found_dram_total; // no need to fix ratio
    double e = (t>=0 ? b/a : a/b)*t;

    return 1.-(a+e*controller->gain);
}
