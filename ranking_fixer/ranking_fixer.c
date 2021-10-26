#include "assert.h"

typedef struct ranking_info {
    double expectedRatioThresh;
    double gain;
    double hotTierSize;
    double coldTierSize;
} ranking_info;

/// @p gain 1) has no effect (0;1) lowers response, (1;inf) amplifies response
void init_ranking_info(
    ranking_info *info, double expected_ratio_thresh, double gain) {
    info->expectedRatioThresh = expected_ratio_thresh;
    info->gain = gain;
    info->hotTierSize = 1-expected_ratio_thresh;
    info->coldTierSize = expected_ratio_thresh;
}

double calculate_fixed_thresh(ranking_info *info, double found_ratio) {
    double diff = found_ratio - info->expectedRatioThresh;

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
    double d = 1-found_ratio;
    double t=a-c;

    double e = (t>=0 ? b/a : a/b)*t;

    return a+e*info->gain;
}

#define assert_close(a, b) do { \
    const double ACCURACY=1e-9; /* arbitrary value */ \
    double diff = a-b; \
    double abs_diff = diff >= 0 ? diff : -diff; \
    assert(abs_diff < ACCURACY && #a " vs " #b); \
} while(0)

void test_ratio_transformations(void) {
    ranking_info info;
    init_ranking_info(&info, 0.7, 1);
    double fixed_thresh;
    // corner cases:
    // ALL PMEM
    fixed_thresh = calculate_fixed_thresh(&info, 1);
    assert_close(fixed_thresh, 0);
    // ALL DRAM
    fixed_thresh = calculate_fixed_thresh(&info, 0);
    assert_close(fixed_thresh, 1);
    // already correct
    fixed_thresh = calculate_fixed_thresh(&info, 0.7);
    assert_close(fixed_thresh, 0.7);
    // 50%
    fixed_thresh = calculate_fixed_thresh(&info, 0.85);
    assert_close(fixed_thresh, 0.35);
    fixed_thresh = calculate_fixed_thresh(&info, 0.35);
    assert_close(fixed_thresh, 0.85);
    // 30%
    fixed_thresh = calculate_fixed_thresh(&info, 0.8);
    assert_close(fixed_thresh, 2./3.*0.7);
    fixed_thresh = calculate_fixed_thresh(&info, 2./3.*0.7);
    assert_close(fixed_thresh, 0.8);
}

void test_ratio_transformations_gain(void) {
    ranking_info info;
    init_ranking_info(&info, 0.7, 2);
    double fixed_thresh;
    // corner cases:
    // ALL PMEM
    fixed_thresh = calculate_fixed_thresh(&info, 1);
    assert_close(fixed_thresh, -0.7);
    // ALL DRAM
    fixed_thresh = calculate_fixed_thresh(&info, 0);
    assert_close(fixed_thresh, 1.3);
    // already correct
    fixed_thresh = calculate_fixed_thresh(&info, 0.7);
    assert_close(fixed_thresh, 0.7);
    // 50%
    fixed_thresh = calculate_fixed_thresh(&info, 0.85);
    assert_close(fixed_thresh, 0);
    fixed_thresh = calculate_fixed_thresh(&info, 0.35);
    assert_close(fixed_thresh, 1);
    // 1/3
    fixed_thresh = calculate_fixed_thresh(&info, 0.8);
    assert_close(fixed_thresh, 1./3.*0.7);
    fixed_thresh = calculate_fixed_thresh(&info, 2./3.*0.7);
    assert_close(fixed_thresh, 0.9);
}

int main(int argc, char *argv[]) {
    // TODO write tests
    test_ratio_transformations();
    test_ratio_transformations_gain();
}
