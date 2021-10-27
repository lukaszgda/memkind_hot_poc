#include "memkind/internal/ranking_fixer.h"

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif


MEMKIND_EXPORT void ranking_fixer_init_ranking_info(
    ranking_info *info, double expected_dram_total, double gain) {
    info->gain = gain;
    info->hotTierSize = expected_dram_total;
    info->coldTierSize = 1-expected_dram_total;
}

MEMKIND_EXPORT double ranking_fixer_calculate_fixed_thresh(ranking_info *info, double found_dram_total) {

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
    double a=info->coldTierSize;
    double b=info->hotTierSize;
    double c = 1-found_dram_total;
    // double d = 1-found_ratio; // unused
    double t=a-c;

    if (a == c && a == 0)
        // corner case - the formula below gives 0/0 (indeterminate form)
        return found_dram_total; // no need to fix ratio
    double e = (t>=0 ? b/a : a/b)*t;

    return 1.-(a+e*info->gain);
}
