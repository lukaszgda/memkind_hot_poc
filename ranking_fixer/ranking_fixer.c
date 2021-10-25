

typedef struct ranking_info {
    double expectedRatioThresh;
    double gain;
    /// sizeFrame[0]: cold tier size
    /// sizeFrame[1]: hot tier size
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

double calculate_fixed_ratio(ranking_info *info, double found_ratio) {
    double diff = found_ratio - info->expectedRatioThresh;

    // |---a----------|-------b--|
    // |---c--|---------------d--|
    // |---c--|-(a-c)-|----------|
    // |---c----------|-e-|------|
    // (a-c)/a==e/b
    // e = b/a * (a-c)
    double a=info->coldTierSize;
    double b=info->hotTierSize;
    double c = found_ratio;
    double d = 1-found_ratio;
    double e = b/a*(a-c);

    return c+e*info->gain;
}

int main(int argc, char *argv[]) {
    // TODO write tests
}
