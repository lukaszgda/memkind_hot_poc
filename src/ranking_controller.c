#include "memkind/internal/ranking_controller.h"

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif


MEMKIND_EXPORT void ranking_controller_init_ranking_controller(
    ranking_controller *controller, double expected_dram_total,
    double proportional_term, double integral_term) {
    controller->integrated_error = 0;
    controller->proportional = proportional_term;
    controller->integral = integral_term;
    ranking_controller_set_expected_dram_total(
        controller, expected_dram_total);
}

MEMKIND_EXPORT void
ranking_controller_set_expected_dram_total(ranking_controller *controller,
                                          double expected_dram_total) {
    controller->hotTierSize = expected_dram_total;
    controller->coldTierSize = 1-expected_dram_total;
}

MEMKIND_EXPORT double
ranking_controller_calculate_fixed_thresh(ranking_controller *controller,
                                          double found_dram_total) {

    // case: found thresh too low
    // |---a----------|--------b--------------|
    // |---c--|----------------d--------------|
    // |---c--|-(a-c)-|-----------------------|
    // |---a----------|-e-|-------------------|
    // |---a----------|-steering_signal-|-out-|
    // |---new_inv_thresh---------------|-out-|
    // (a-c)/a==e/b
    // e = b/a * (a-c)

    // case: found thresh too high
    // |---a-----------------|--------b--------|
    // |---c-------------------------|-d-------|
    // |---c-------------------------|-d-------|
    // |---c-----------------|-(c-a)-|---------|
    // |--------------|--e---|-----------------| *e* is negative
    // |---|-steering_signal-|-----------------| *steering_signal* is negative
    // |xxx|-------------------------out-------|
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

    // euler forward integration with timestep = 1
    controller->integrated_error += e;  // it's really this simple

    double steering_signal =
        e*controller->proportional
        + controller->integrated_error * controller->integral;
    double new_inv_thresh = a+ steering_signal;
    
    double out = 1.0 - new_inv_thresh;
    // restrict to values in a closed range <0,1>
    if (out > 1.0)
        out = 1.0;
    else if (out < 0)
        out = 0.0;

    return out;
}
