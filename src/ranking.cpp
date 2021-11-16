extern "C" {
#include "memkind/internal/ranking.h"
#include "memkind/internal/wre_avl_tree.h"
}

#include "memkind/internal/slab_allocator.h"
#include "memkind/internal/ranking_controller.h"

#include <jemalloc/jemalloc.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#include <cmath>

#include "memkind/internal/memkind_private.h"
#include "memkind/internal/memkind_log.h"
#include "memkind/internal/memkind_memtier.h"

#define abs(x) ((x) >= 0 ? (x) : -(x))

#if CHECK_ADDED_SIZE
extern size_t g_total_ranking_size;
#endif

// TODO extern ttypes cannot stay like that!!!
// this should be fixed once custom allocator is created
extern struct ttype *ttypes;

// #define THREAD_SAFE
// #define THREAD_CHECKER

// OFFLOAD_RANKING_OPS_TO_BACKGROUD_THREAD: ranking is only accessed from pebs
// !OFFLOAD_RANKING_OPS_TO_BACKGROUD_THREAD: ranking is accessed from:
//      - pebs (touch),
//      - any thread that calls malloc, realloc, free, etc.
// mutex is required when !OFFLOAD_RANKING_OPS_TO_BACKGROUD_THREAD
#if defined(THREAD_SAFE) || !(OFFLOAD_RANKING_OPS_TO_BACKGROUD_THREAD)
#ifdef THREAD_CHECKER

class recursion_counter {
    static thread_local int counter;
public:
    recursion_counter() {
        counter++;
        assert(counter == 1);
    }
    ~recursion_counter() {
        counter--;
        assert(counter == 0);
    }
};

thread_local int recursion_counter::counter=0;

// #define RANKING_LOCK_GUARD(ranking) std::lock_guard<std::mutex> lock_guard((ranking)->mutex)
#define RANKING_LOCK_GUARD(ranking) /* do { */                       \
    static thread_local bool thread_counted=false;              \
    static std::atomic<int> thread_counter(0);                  \
    recursion_counter counter;                                  \
    if (!thread_counted) {                                      \
        thread_counter++;                                       \
        assert(thread_counter == 1);                            \
        thread_counted = true;                                  \
    }                                                           \
    std::lock_guard<std::mutex> lock_guard((ranking)->mutex);   \
/* } while (0) */
#else /* ndef THREAD_CHECKER */
#define RANKING_LOCK_GUARD(ranking) \
    std::lock_guard<std::mutex> lock_guard((ranking)->mutex)
// lockguard cannot go into do-while(0), its scope would end within the macro!
#endif
#else /* ndef THREAD_SAFE */
#define RANKING_LOCK_GUARD(ranking) (void)(ranking)->mutex
#endif

using namespace std;

struct ranking {
    std::atomic<thresh_t> hotThreshold;
    wre_tree_t *entries;
    slab_alloc_t aggHotAlloc;
    std::mutex mutex;
    double oldWeight;
    double newWeight;
    ranking_controller controller;
};

typedef struct AggregatedHotness {
    size_t size;
    quantified_hotness_t quantifiedHotness;
} AggregatedHotness_t;

//--------private function implementation---------

static bool is_hotter_agg_hot(const void *a, const void *b)
{
    int a_hot = ((AggregatedHotness_t *)a)->quantifiedHotness;
    int b_hot = ((AggregatedHotness_t *)b)->quantifiedHotness;
    return a_hot > b_hot;
}

static void ranking_create_internal(ranking_t **ranking, double old_weight);
static void ranking_destroy_internal(ranking_t *ranking);
// static void ranking_add_internal(ranking_t *ranking, const struct tblock *block);
static void ranking_add_internal(ranking_t *ranking, double hotness, size_t size);
/// Attempt to remove from ranking the entry
/// If entry does not exist or has insufficient size,
/// remove all that can be removed and return the removed size
///
/// @return the size that was actually removed
/// @warning @todo remove @p entry !!! Can be done once custom allocator is ready
static size_t ranking_remove_internal_relaxed(ranking_t *ranking,
                                              const struct ttype *entry);
// static void ranking_remove_internal(ranking_t *ranking,
//                                     const struct tblock *block);
static void
ranking_remove_internal(ranking_t *ranking, double hotness, size_t size);
static thresh_t ranking_get_hot_threshold_internal(ranking_t *ranking);
static thresh_t
ranking_calculate_hot_threshold_dram_total_internal(
    ranking_t *ranking, double dram_total_ratio, double dram_total_used_ratio);
static thresh_t
ranking_calculate_hot_threshold_dram_pmem_internal(
    ranking_t *ranking, double dram_pmem_ratio, double dram_pmem_used_ratio);
// static bool ranking_is_hot_internal(ranking_t *ranking, struct ttype *entry);
// static void ranking_update_internal(ranking_t *ranking,
//                                     struct ttype *entry_to_update,
//                                     const struct ttype *updated_values);
static void ranking_touch_entry_internal(ranking_t *ranking,
                                         struct ttype *entry,
                                         uint64_t timestamp,
                                         double add_hotness);

static void ranking_touch_internal(ranking_t *ranking, struct ttype *entry,
                                   uint64_t timestamp, double add_hotness);

/// Reduce the number of possible hotness steps
static quantified_hotness_t ranking_quantify_hotness(double hotness);

static double ranking_dequantify_hotness(quantified_hotness_t quantified_hotness);

//--------private function implementation---------

static quantified_hotness_t ranking_quantify_hotness(double hotness)
{
#if QUANTIFICATION_ENABLED
    return int(log(hotness));
#else
    return hotness;
#endif
}

static double ranking_dequantify_hotness(quantified_hotness_t quantified_hotness)
{
#if QUANTIFICATION_ENABLED
    return exp(quantified_hotness);
#else
    return quantified_hotness;
#endif
}

#include <limits>

// TODO should probably be static; exported only for tests
MEMKIND_EXPORT double
ranking_update_coeffs(double *hotness_history_coeffs, double seconds_diff,
                      double add_hotness) {
    assert(seconds_diff >= 0 && "timestamps are not monotonic!");
    double ret = 0;
    for (size_t i=0; i<EXPONENTIAL_COEFFS_NUMBER; ++i) {
        double temp = pow(EXPONENTIAL_COEFFS_VALS[i], seconds_diff);
        assert(temp <= 1 && temp >= 0 && "exponential coeff is incorrect!");
        hotness_history_coeffs[i] *= temp;
        double add = EXPONENTIAL_COEFFS_CONMPENSATION_COEFFS[i]*add_hotness;
        assert(add>=0);
        const double MAX_DBL = std::numeric_limits<double>::max();
        // check for double overflow CAUTION: don't overflow in checking code!
        if (MAX_DBL - add > hotness_history_coeffs[i])
            hotness_history_coeffs[i] +=
                EXPONENTIAL_COEFFS_CONMPENSATION_COEFFS[i]*add_hotness;
        else
            hotness_history_coeffs[i] = MAX_DBL;
        // check for double overflow - again
        if (MAX_DBL-ret > hotness_history_coeffs[i])
            ret += hotness_history_coeffs[i];
        else
            ret = MAX_DBL;
    }

    return ret;
}

// old touch entry definition - as described in design doc
static void
ranking_touch_entry_internal(ranking_t *ranking, struct ttype *entry,
                                  uint64_t timestamp, double add_hotness)
{
    if (entry->touchCb)
        entry->touchCb(entry->touchCbArg);

    assert(add_hotness>=0);

#if HOTNESS_POLICY == HOTNESS_POLICY_TOTAL_COUNTER

    entry->f += add_hotness;

#elif HOTNESS_POLICY == HOTNESS_POLICY_TIME_WINDOW

    assert(entry->n1>=0);
    assert(entry->n2>=0);
    if (entry->touchCb)
        entry->touchCb(entry->touchCbArg);

    entry->n1 += add_hotness;
    entry->t0 = timestamp;
    if (timestamp != 0) {
        if (entry->timestamp_state == TIMESTAMP_NOT_SET) {
            entry->t2 = timestamp;
            entry->timestamp_state = TIMESTAMP_INIT;
        }

        if (entry->timestamp_state == TIMESTAMP_INIT_DONE) {
            if ((entry->t0 - entry->t1) > hotness_measure_window) {
                // move to next measurement window
                float f2 = ((float)entry->n2) / (entry->t1 - entry->t2);
                float f1 = ((float)entry->n1) / (entry->t0 - entry->t1);
                entry->f = f2 * ranking->oldWeight + f1 * ranking->newWeight;
                entry->t2 = entry->t1;
                entry->t1 = entry->t0;
                // TODO - n2 should be calclated differently
                entry->n2 = entry->n1;
                entry->n1 = 0;
                //                 printf("wre: hotness updated: [new, old]:
                //                 [%.16f, %.16f]\n", f1, f2);
            }
            //             printf("wre: hotness awaiting window\n");
        } else {
            // TODO init not done
            //             printf("wre: hotness awaiting window\n");
            if ((entry->t0 - entry->t2) > hotness_measure_window) {
                // TODO - classify hotness
                entry->timestamp_state = TIMESTAMP_INIT_DONE;
                entry->t1 = entry->t0;
                entry->n2 = entry->n1;
                //                 printf("wre: hotness init done\n");
            }
        }
    } else {
        //         printf("wre: hotness touch without timestamp!\n");
    }
    assert(entry->f >= 0);
    assert(entry->n1 >= 0);
//     assert(entry->n0 >= 0);
#elif HOTNESS_POLICY == HOTNESS_POLICY_EXPONENTIAL_COEFFS
    double seconds_diff = (timestamp - entry->t0)/1000000000.0;
    entry->t0 = timestamp;
    assert(seconds_diff >= 0 && "timestamps are not monotonic!");
    entry->f =
        ranking_update_coeffs(entry->hotness_history_coeffs, seconds_diff,
                              add_hotness);
#else
    assert(false && "Unknown policy");
#endif
}


void ranking_create_internal(ranking_t **ranking, double old_weight)
{
    *ranking = (ranking_t *)jemk_malloc(sizeof(ranking_t));
    wre_create(&(*ranking)->entries, is_hotter_agg_hot);
    // placement new for mutex
    // mutex is already inside a structure, so alignment should be ok
    (void)new ((void *)(&(*ranking)->mutex)) std::mutex();
    int ret =
        slab_alloc_init(&(*ranking)->aggHotAlloc, sizeof(AggregatedHotness_t), 0);
//     thresh_t init_thresh;
//     init_thresh.threshVal = 0.;
//     init_thresh.threshValid = false;
    (*ranking)->hotThreshold = { 0., false };
    (*ranking)->oldWeight = old_weight;
    (*ranking)->newWeight = 1 - old_weight;
    ranking_controller_init_ranking_controller(
        &(*ranking)->controller, 0.5 /* unknown at this point */,
        CONTROLLER_PROPORTIONAL_GAIN, CONTROLLER_INTEGRAL_GAIN);
    assert(ret == 0 && "slab allocator initialization failed!");
}

void ranking_destroy_internal(ranking_t *ranking)
{
    // explicit destructor call for mutex
    // which was created with placement new
    ranking->mutex.~mutex();
    slab_alloc_destroy(&ranking->aggHotAlloc);
    jemk_free(ranking);
}

static thresh_t ranking_get_hot_threshold_internal(ranking_t *ranking)
{
    return ranking->hotThreshold;
}

thresh_t
ranking_calculate_hot_threshold_dram_total_internal(
    ranking_t *ranking, double dram_total_ratio, double dram_total_used_ratio)
{
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp_size = ranking_calculate_total_size(ranking);
    wre_tree_t *temp_cpy;
    wre_clone(&temp_cpy, ranking->entries);
#endif

#if RANKING_CONTROLLER_ENABLED
    // TODO add tests for this one?
    ranking_controller_set_expected_dram_total(&ranking->controller,
                                               dram_total_ratio);
    double fixed_dram_total_ratio =
        ranking_controller_calculate_fixed_thresh(&ranking->controller,
                                                  dram_total_used_ratio);
#if PRINT_ADJUSTED_RATIO_INFO
    uint32_t counter=0;
    if (++counter > PRINT_RATIO_ADJUSTED_INTERVAL) {
        log_info("controller: ratio adjusted [%f to %f]",
            dram_total_ratio, fixed_dram_total_ratio);
        counter = 0;
    }
#endif
    dram_total_ratio = fixed_dram_total_ratio;

#endif

    bool result_valid = false;
#if INTERPOLATED_THRESH
    wre_interpolated_result_t ret = wre_find_weighted_interpolated(
            ranking->entries, dram_total_ratio);
    AggregatedHotness_t agg_hot_={0};
    AggregatedHotness_t *agg_hot = NULL;
    // TODO refactor - only one ret.left or fallbackRequired check
    if (ret.left) {
        if (ret.fallbackRequired) {
            // TODO fallback to static ratio
            assert(!result_valid && "incorrect init!");
        } else {
            double hotness_left = ((AggregatedHotness_t*)ret.left)->quantifiedHotness;
            double hotness_right =
                ret.right ? ((AggregatedHotness_t*)ret.right)->quantifiedHotness : hotness_left+1; // TODO add coeff
    //         agg_hot_.size = 0u; // irrelevant
            agg_hot_.quantifiedHotness = hotness_left + (hotness_right-hotness_left)*ret.percentage,

            agg_hot = &agg_hot_;
            result_valid=true;
        }
    }
#else
    AggregatedHotness_t *agg_hot = (AggregatedHotness_t *)wre_find_weighted(
        ranking->entries, dram_total_ratio);
    result_valid = true;
#endif
    thresh_t thresh;
    thresh.threshVal= 0.;
    thresh.threshValid= false;
    if (agg_hot && result_valid) {
        thresh.threshVal =
            ranking_dequantify_hotness(agg_hot->quantifiedHotness);
        thresh.threshValid = true;
    }
    // set thresh even if invalid - static ratio fallback should happen
    // in this situation
    ranking->hotThreshold = thresh;

    // TODO remove this!!!
    //     printf("wre: threshold_dram_total_internal\n");
    // EOF TODO
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t after_size = wre_calculate_total_size(ranking->entries);
    assert(temp_size == after_size);
    wre_destroy(temp_cpy);
#endif

    return ranking_get_hot_threshold_internal(ranking);
}

static thresh_t
ranking_calculate_hot_threshold_dram_pmem_internal(
    ranking_t *ranking, double dram_pmem_ratio, double dram_pmem_used_ratio)
{
    double ratio = dram_pmem_ratio / (1 + dram_pmem_ratio);
    double ratio_used = dram_pmem_ratio / (1 + dram_pmem_ratio);
    return ranking_calculate_hot_threshold_dram_total_internal(
        ranking, ratio, ratio_used);
}

// void ranking_add_internal(ranking_t *ranking, const struct tblock *block)
// {
//     int tidx = block->type;
//     assert(tidx != -1 && tidx >=0 && "tidx invalid!");
//     struct ttype *entry = &ttypes[tidx];
//     AggregatedHotness temp;
//     temp.hotness = entry->f; // only hotness matters for lookup // TODO: rrudnick ????
//     AggregatedHotness_t *value =
//         (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
//     if (value) {
//         value with the same hotness is already there, should be aggregated
//         value->size += block->size;
//     } else {
//         value = (AggregatedHotness_t *)jemk_malloc(sizeof(AggregatedHotness_t));
//         value->hotness = entry->f;
//         value->size = block->size;
//                 printf("wre: hotness not found, adds\n");
//     }
//     if (value->size > 0)
//         wre_put(ranking->entries, value, value->size);
//     else
//         jemk_free(value);
// }

void ranking_add_internal(ranking_t *ranking, double hotness, size_t size)
{
    AggregatedHotness temp;
    // only hotness matters for lookup // TODO: rrudnick ????
    temp.quantifiedHotness = ranking_quantify_hotness(hotness);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp_size = wre_calculate_total_size(ranking->entries);
    wre_tree_t *temp_cpy;
    wre_clone(&temp_cpy, ranking->entries);
#endif
    AggregatedHotness_t *value =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t after_size = wre_calculate_total_size(ranking->entries);
    if (value) 
        assert(temp_size == after_size+value->size);
    else
        assert(temp_size == after_size);
    wre_destroy(temp_cpy);
#endif
    if (value) {
        // value with the same hotness is already there, should be aggregated
        value->size += size;
    } else {
//         value = (AggregatedHotness_t *)jemk_malloc(sizeof(AggregatedHotness_t));
        value = (AggregatedHotness_t *)slab_alloc_malloc(&ranking->aggHotAlloc);
        value->quantifiedHotness = ranking_quantify_hotness(hotness);
        value->size = size;
    }
    if (value->size > 0) {
        wre_put(ranking->entries, value, value->size);
#if CHECK_ADDED_SIZE
        g_total_ranking_size += size;    
        log_info("wre_put: %ld g_total_ranking_size %ld", 
            size, g_total_ranking_size);
#endif
    }
    else
        slab_alloc_free(value);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t after_after_size = wre_calculate_total_size(ranking->entries);
    assert(temp_size + size == after_after_size);
#endif
}

thresh_t ranking_get_thresh(ranking_t *ranking) {
    // no need for lock
    return ranking_get_hot_threshold_internal(ranking);
}

bool ranking_is_hot_internal(ranking_t *ranking, struct ttype *entry)
{
    thresh_t thresh = ranking_get_hot_threshold_internal(ranking);
    (void)thresh.threshValid;
    return entry->f > thresh.threshVal;
}

static size_t ranking_remove_internal_relaxed(ranking_t *ranking, const struct ttype *entry)
{
    size_t ret = 0;
    AggregatedHotness temp;
    // only hotness matters for lookup
    temp.quantifiedHotness = ranking_quantify_hotness(entry->f);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp_size = wre_calculate_total_size(ranking->entries);
    wre_tree_t *temp_cpy;
    wre_clone(&temp_cpy, ranking->entries);
#endif
    AggregatedHotness_t *removed =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t after_size = wre_calculate_total_size(ranking->entries);
    if (removed) 
        assert(temp_size == after_size + removed->size);
    else
        assert(temp_size == after_size);
    wre_destroy(temp_cpy);
#endif
    // needs to put back as much as was removed,
    // even if the entry gets modified in the meantime
    size_t block_size = entry->total_size;
    if (removed) {
        if (block_size > removed->size)
        {
            ret = removed->size;
            removed->size = 0;
        } else {
            ret = block_size;
            removed->size -= block_size;
        }
        if (removed->size == 0)
            slab_alloc_free(removed);
        else {
            wre_put(ranking->entries, removed, removed->size);
#if CHECK_ADDED_SIZE
            // only for asserts
            size_t after_after_size = wre_calculate_total_size(ranking->entries);
            assert(after_after_size == after_size + removed->size);
#endif
        }
    } else {
        assert(entry->total_size == 0);
        ret = 0; // defensive programming - nothing found, nothing removed
    }
#if CHECK_ADDED_SIZE
    (void)wre_calculate_total_size(ranking->entries); // only for asserts
    
    g_total_ranking_size -= ret;
    log_info("wre_remove: -%ld g_total_ranking_size %ld", 
        ret, g_total_ranking_size);
#endif

    return ret;
}

void ranking_remove_internal(ranking_t *ranking, double hotness, size_t size)
{
    if (size == 0u) // nothing to do
    {
        log_info("ranking_remove_internal size == 0");
        return;
    }

    AggregatedHotness temp;
    // only hotness matters for lookup
    temp.quantifiedHotness = ranking_quantify_hotness(hotness);

#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp_size = wre_calculate_total_size(ranking->entries);
    wre_tree_t *temp_cpy;
    wre_clone(&temp_cpy, ranking->entries);
#endif
    AggregatedHotness_t *removed =
        (AggregatedHotness_t *)wre_remove(ranking->entries, &temp);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t after_size = wre_calculate_total_size(ranking->entries);
    if (removed)
        assert(after_size + removed->size == temp_size);
    else
        assert(after_size == temp_size);
    wre_destroy(temp_cpy);
#endif
    if (removed) {
        if (size > removed->size)
        {
            log_fatal("ranking_remove_internal: tried to remove more than added (%lu vs %lu)!", size, removed->size);
#if CRASH_ON_BLOCK_NOT_FOUND
            assert(false && "attempt to remove non-existent data!");
#endif
        }
        removed->size -= size;
        if (removed->size == 0)
            slab_alloc_free(removed);
        else 
            wre_put(ranking->entries, removed, removed->size);
    } else {
#if CRASH_ON_BLOCK_NOT_FOUND
        assert(false && "dealloc non-allocated block!"); // TODO remove!
        assert(false && "attempt to remove non-existent data!");
#endif

#if CHECK_ADDED_SIZE
        log_info("ranking_remove_internal attempt to remove non-existent data! "
            "g_total_ranking_size %ld", g_total_ranking_size);
#endif
    }
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t after_after_size = wre_calculate_total_size(ranking->entries);
    assert(after_after_size + size == temp_size);

    g_total_ranking_size -= size;    
    log_info("wre_remove: %ld g_total_ranking_size %ld", 
        size, g_total_ranking_size);
#endif
}

// static void ranking_update_internal(ranking_t *ranking,
//                                     struct ttype *entry_to_update,
//                                     const struct ttype *updated_values)
// {
//     ranking_remove_internal(ranking, entry_to_update);
//     memcpy(entry_to_update, updated_values, sizeof(*entry_to_update));
//     ranking_add_internal(ranking, entry_to_update);
// }
// TODO cleanup
static void ranking_touch_internal(ranking_t *ranking, struct ttype *entry,
                                   uint64_t timestamp, double add_hotness)
{
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp0_size = wre_calculate_total_size(ranking->entries);
#endif
    size_t removed = ranking_remove_internal_relaxed(ranking, entry);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp1_size = wre_calculate_total_size(ranking->entries);
    assert(temp1_size + removed == temp0_size);
#endif
    // touch true entry
    ranking_touch_entry_internal(ranking, entry, timestamp, add_hotness);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp2_size = wre_calculate_total_size(ranking->entries);
    assert(temp2_size == temp1_size);
#endif
    // add data back to ranking - as much as was removed
    ranking_add_internal(ranking, entry->f, removed);
#if CHECK_ADDED_SIZE
    // only for asserts
    size_t temp3_size = wre_calculate_total_size(ranking->entries);
    assert(temp3_size == temp0_size);
    assert(temp3_size == temp2_size+removed);
#endif
}

//--------public function implementation---------

MEMKIND_EXPORT void ranking_create(ranking_t **ranking, double old_weight)
{
    ranking_create_internal(ranking, old_weight);
}

MEMKIND_EXPORT void ranking_destroy(ranking_t *ranking)
{
    ranking_destroy_internal(ranking);
}

MEMKIND_EXPORT thresh_t ranking_get_hot_threshold(ranking_t *ranking)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    return ranking_get_hot_threshold_internal(ranking);
}

MEMKIND_EXPORT thresh_t
ranking_calculate_hot_threshold_dram_total(ranking_t *ranking,
                                           double dram_total_ratio,
                                           double dram_total_used_ratio)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    return ranking_calculate_hot_threshold_dram_total_internal(
        ranking, dram_total_ratio, dram_total_used_ratio);
}

MEMKIND_EXPORT thresh_t
ranking_calculate_hot_threshold_dram_pmem(ranking_t *ranking,
                                          double dram_pmem_ratio,
                                          double dram_pmem_used_ratio)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    return ranking_calculate_hot_threshold_dram_pmem_internal(
        ranking, dram_pmem_ratio, dram_pmem_used_ratio);
}

// MEMKIND_EXPORT void ranking_add(ranking_t *ranking, struct tblock *block)
// {
//     RANKING_LOCK_GUARD(ranking);
//     ranking_add_internal(ranking, block);
// }

MEMKIND_EXPORT void ranking_add(ranking_t *ranking, double hotness, size_t size)
{
    RANKING_LOCK_GUARD(ranking);
    ranking_add_internal(ranking, hotness, size);
}

MEMKIND_EXPORT bool ranking_is_hot(ranking_t *ranking, struct ttype *entry)
{
    // mutex not necessary
    // std::lock_guard<std::mutex> lock_guard(ranking->mutex);
//     RANKING_LOCK_GUARD(ranking);
    return ranking_is_hot_internal(ranking, entry);
}

MEMKIND_EXPORT void
ranking_remove(ranking_t *ranking, double hotness, size_t size)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    ranking_remove_internal(ranking, hotness, size);
}

// MEMKIND_EXPORT void ranking_update(ranking_t *ranking,
//                                    struct ttype *entry_to_update,
//                                    const struct ttype *updated_value)
// {
//     RANKING_LOCK_GUARD(ranking);
//     ranking_update_internal(ranking, entry_to_update, updated_value);
// }

MEMKIND_EXPORT void ranking_touch(ranking_t *ranking, struct ttype *entry,
                                  uint64_t timestamp, double add_hotness)
{
    //     printf("touches ranking, timestamp: [%lu]\n", timestamp);
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    ranking_touch_internal(ranking, entry, timestamp, add_hotness);
}

MEMKIND_EXPORT void ranking_set_touch_callback(ranking_t *ranking,
                                               tachanka_touch_callback cb,
                                               void *arg, struct ttype *type)
{
//     std::lock_guard<std::mutex> lock_guard(ranking->mutex);
    RANKING_LOCK_GUARD(ranking);
    type->touchCb = cb;
    type->touchCbArg = arg;
}

size_t ranking_calculate_total_size(ranking_t *ranking)
{
    return wre_calculate_total_size(ranking->entries);
}

