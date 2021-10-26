#include "memkind/internal/ranking_fixer.h"

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif


MEMKIND_EXPORT void ranking_fixer_init_ranking_info(
    ranking_info *info, double expected_ratio_thresh, double gain) {
    info->expectedRatioThresh = expected_ratio_thresh;
    info->gain = gain;
    info->hotTierSize = 1-expected_ratio_thresh;
    info->coldTierSize = expected_ratio_thresh;
}

MEMKIND_EXPORT double ranking_fixer_calculate_fixed_thresh(ranking_info *info, double found_ratio) {

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
    double c = found_ratio;
    // double d = 1-found_ratio; // unused
    double t=a-c;

    double e = (t>=0 ? b/a : a/b)*t;

    return a+e*info->gain;
}
