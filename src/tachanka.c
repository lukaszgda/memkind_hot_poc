
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_memtier.h>
#include <memkind/internal/memkind_log.h>
#include <memkind/internal/bthash.h>
#include <memkind/internal/critnib.h>
#include <memkind/internal/tachanka.h>
#include <memkind/internal/ranking.h>
#include <memkind/internal/lockless_srmw_queue.h>
#include <memkind/internal/bigary.h>
#include <memkind/internal/wre_avl_tree.h>
#include <memkind/internal/slab_allocator.h>
#include <memkind/internal/heatmap.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <assert.h>
#include <string.h>
#include "unistd.h"
#include <fcntl.h>
#include <signal.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>         /* Definition of SYS_* constants */
#include <sys/types.h>


// DEBUG
#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

// struct ttype *ttypes;
// struct tblock *tblocks;
static slab_alloc_t tblock_alloc;
static slab_alloc_t ttype_alloc;



// TODO (possibly) move elsewhere - make sure multiple rankings are supported!
static ranking_t *ranking;
static lq_buffer_t ranking_event_buff;
static _Atomic double g_dramToTotalDesiredRatio=1.0;
static _Atomic double g_dramToTotalActualRatio=1.0;
/*static*/ critnib *hash_to_type, *addr_to_block;

#define ADD(var,x) __sync_fetch_and_add(&(var), (x))
#define SUB(var,x) __sync_fetch_and_sub(&(var), (x))

#if CHECK_ADDED_SIZE
size_t g_total_critnib_size=0u;
size_t g_total_ranking_size=0u;
#endif

static void check_dram_total_ratio(double ratio) {
    if (ratio < 0 || ratio > 1) {
        log_fatal("Incorrect ratio [%f], exiting", ratio);
        exit(-1);
    }
}

static int aggregate_ttypes(uintptr_t key, void *value, void *privdata) {
    (void)key;
    struct ttype *cttype = value;
    heatmap_aggregator_t *aggregator = privdata;
    HeatmapEntry_t temp_entry = {
        .dram_to_total=cttype->dram_size/(double)cttype->total_size,
        .hotness=cttype->f,
    };
    heatmap_aggregator_aggregate(aggregator, &temp_entry);

    return 0;
}

void register_block(uint64_t hash, void *addr, size_t size, bool is_hot)
{
#if CHECK_ADDED_SIZE
    if (g_total_ranking_size != g_total_critnib_size) {
        log_info("rank %ld, crit: %ld", g_total_ranking_size, g_total_critnib_size);
    }

    if (g_total_ranking_size != g_total_critnib_size) 
    {
        log_info("g_total_ranking_size != g_total_critnib_size");
    }
    assert(g_total_ranking_size == g_total_critnib_size);
#endif

    //printf("hash: %lu\n", hash);

    struct ttype *t = critnib_get(hash_to_type, hash);
    if (!t) {
        t = slab_alloc_malloc(&ttype_alloc);
        memset(t, 0, sizeof(t[0]));

        t->hash = hash;
        t->total_size = 0; // will be incremented later
        t->dram_size = 0; // will be incremented later
        t->timestamp_state = TIMESTAMP_NOT_SET;

        int ret = critnib_insert(hash_to_type, hash, t, false);
        if (ret == EEXIST) {
            slab_alloc_free(t);
            log_info("critnib_insert EEXIST");
            t = critnib_get(hash_to_type, hash); // raced with another thread
            if (!t) {
                log_fatal("Alloc type disappeared?!?");
                exit(-1);
            }
        }
        t->f = EXPONENTIAL_COEFFS_NUMBER*HOTNESS_INITIAL_SINGLE_VALUE;
#if PRINT_POLICY_LOG_STATISTICS_INFO && PRINT_POLICY_LOG_DETAILED_TYPE_INFO
        static atomic_uint_fast64_t counter=0;
        counter++;
        log_info("new type created, total types: %lu", counter);
#endif
    } // else: t is ok

    t->num_allocs++;
    t->total_size+= size;
    if (is_hot)
        t->dram_size += size;

    struct tblock *bl = slab_alloc_malloc(&tblock_alloc);

    bl->addr = addr; // TODO do we need to store addr separately?
    bl->size = size;
    bl->type = t;
    bl->is_hot = is_hot;

#if PRINT_CRITNIB_NEW_BLOCK_REGISTERED_INFO
    log_info("New block %d registered: addr %p size %lu type %d", fb, (void*)addr, size, nt);
#endif

    int ret = critnib_insert(addr_to_block, (uintptr_t)addr, bl, false);
#if CHECK_ADDED_SIZE
    if (ret == EEXIST) 
    {
        log_info("!!!!!!!!! register_block: critnib_insert(addr_to_block, %d) == EEXIST, "
            "g_total_ranking_size %ld g_total_critnib_size %ld", fb, g_total_ranking_size, g_total_critnib_size);
    }

    g_total_critnib_size += bl->size;
    log_info("critnib_insert addr_to_block leaf %d size %ld g_total_critnib_size %ld",
        fb, bl->size, g_total_critnib_size);
#else
    (void)ret;
#endif
}

void realloc_block(void *addr, void *new_addr, size_t size)
{
    struct tblock *bl = critnib_remove(addr_to_block, (intptr_t)addr);
    if (!bl)
    {
#if PRINT_CRITNIB_NOT_FOUND_ON_REALLOC_WARNING
        log_info("WARNING: Tried realloc a non-allocated block at %p", addr);
#endif
#if CRASH_ON_BLOCK_NOT_FOUND
        assert(false && "dealloc non-allocated block!"); // TODO remove!
#endif
        return;
    }

#if CHECK_ADDED_SIZE 
    g_total_critnib_size -= bl->size;
#endif

#if PRINT_CRITNIB_REALLOC_INFO
    log_info("realloc %p -> %p (block %d, type %d)", addr, new_addr, bln, bl->type);
#endif

    bl->addr = new_addr;
    struct ttype *t = bl->type;
    SUB(t->total_size, bl->size);
    ADD(t->total_size, size);

    int ret = critnib_insert(addr_to_block, (uintptr_t)new_addr, bl, false);
#if CHECK_ADDED_SIZE
    if (ret == EEXIST) {
        log_info("!!!!!!!!! register_block: critnib_insert(addr_to_block, %d) == EEXIST, "
            "g_total_ranking_size %ld g_total_critnib_size %ld", 
            bln, g_total_ranking_size, g_total_critnib_size);
    } else {
        g_total_critnib_size += bl->size;
        log_info("critnib_insert addr_to_block leaf %d size %ld g_total_critnib_size %ld",
            bln, bl->size, g_total_critnib_size);
    }
#else
    (void)ret;
#endif

    // TODO the new block might have completely different hash/type ...
}

void register_block_in_ranking(void * addr, size_t size)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uint64_t)addr);
    if (!bl) {
#if PRINT_CRITNIB_NOT_FOUND_ON_UNREGISTER_BLOCK_WARNING
        log_info("WARNING: Tried searching for a non-allocated block at %p", addr);
#endif
#if CRASH_ON_BLOCK_NOT_FOUND
        assert(false && "only existing blocks can be registered in ranking!");
#endif
        return;
    }
    assert(bl->type);

#if CHECK_ADDED_SIZE
    volatile size_t pre_real_ranking_size = ranking_calculate_total_size(ranking);
#endif
    ranking_add(ranking, bl->type->f, size);
#if CHECK_ADDED_SIZE
    volatile size_t real_ranking_size = ranking_calculate_total_size(ranking);
    assert(g_total_ranking_size == real_ranking_size);
    assert(pre_real_ranking_size + size == real_ranking_size);
//     wre_destroy(temp_cpy);
#endif
}

void unregister_block(void *addr)
{
    struct tblock *bl = critnib_remove(addr_to_block, (intptr_t)addr);
    if (!bl)
    {
#if PRINT_CRITNIB_NOT_FOUND_ON_UNREGISTER_BLOCK_WARNING
        log_info("WARNING: Tried deallocating a non-allocated block at %p", addr);
#endif

#if CRASH_ON_BLOCK_NOT_FOUND
        assert(false && "dealloc non-allocated block!"); // TODO remove!
#endif
        return;
    }

    struct ttype *t = bl->type;

#if CHECK_ADDED_SIZE
    g_total_critnib_size -= bl->size;
#endif

    SUB(t->num_allocs, 1);
    assert(t->num_allocs >= 0);
    SUB(t->total_size, bl->size);
    assert(t->total_size >= 0);
    if (bl->is_hot)
        SUB(t->dram_size, bl->size);
    assert(t->dram_size >= 0);
    
#if PRINT_CRITNIB_UNREGISTER_BLOCK_INFO
    log_info("Block unregistered: %d addr %p size %lu type %d h %f", 
        bln, (void*)addr, bl->size, bl->type, t->f);
#endif

    ranking_remove(ranking, t->f, bl->size);

    // TODO is resetting to 0 really necessary?
    bl->addr = 0;
    bl->size = 0;
    bl->type = 0;
    slab_alloc_free(bl);

#if CHECK_ADDED_SIZE
    if (g_total_ranking_size != g_total_critnib_size) {
        log_info("rank %ld, crit: %ld", g_total_ranking_size, g_total_critnib_size);
    }
    if (g_total_ranking_size != g_total_critnib_size) 
    {
        log_info("g_total_ranking_size != g_total_critnib_size");
    }
    assert(g_total_ranking_size == g_total_critnib_size);
#endif
}

MEMKIND_EXPORT Hotness_e tachanka_get_hotness_type(const void *addr)
{
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);

    if (!bl || addr >= bl->addr + bl->size)
        return HOTNESS_NOT_FOUND;
    struct ttype *t = bl->type;

    //printf("get_hotness block %d, type %d hot %g\n", bln, tblocks[bln].type, ttypes[tblocks[bln].type].f);

    thresh_t thresh = ranking_get_hot_threshold(ranking);
    if (!thresh.threshValid || t->f == thresh.threshVal)
        return HOTNESS_NOT_FOUND;

    if (t->f > thresh.threshVal)
        return HOTNESS_HOT;
    // (t->f < thresh.threshVal)
    return HOTNESS_COLD;
}

MEMKIND_EXPORT double tachanka_get_hot_thresh(void)
{
    return ranking_get_hot_threshold(ranking).threshVal;
}

MEMKIND_EXPORT Hotness_e tachanka_get_hotness_type_hash(uint64_t hash)
{
    Hotness_e ret = HOTNESS_NOT_FOUND;
    struct ttype *t = critnib_get(hash_to_type, hash);
    if (t) {
        thresh_t thresh = ranking_get_hot_threshold(ranking);
        if (!thresh.threshValid || t->f == thresh.threshVal)
            ret = HOTNESS_NOT_FOUND;
        else if (t->f > thresh.threshVal)
            ret = HOTNESS_HOT;
        else {
            assert(t->f < thresh.threshVal);
            ret = HOTNESS_COLD;
        }
    }
#if PRINT_POLICY_LOG_DETAILED_TYPE_INFO
    else log_info("not found, hash %lu", hash);
#endif

    return ret;
}

/// @warning NOT THREAD SAFE
/// This function operates on block that should not be freed/modifed
/// in the meantime
MEMKIND_EXPORT void touch(void *addr, __u64 timestamp, int from_malloc)
{
#if CHECK_ADDED_SIZE
    assert(g_total_ranking_size == ranking_calculate_total_size(ranking));
#endif
    struct tblock *bl = critnib_find_le(addr_to_block, (uint64_t)addr);
#if PRINT_POLICY_LOG_TOUCH_STATISTICS
    static uint64_t all_touches=0;
    static uint64_t successful_touches=0;
    static uint64_t counter=0;
    ++all_touches;
    if (bl)
        ++successful_touches;
    if (++counter>PRINT_TOUCH_STATISTICS_INTERVAL) {
        log_info("touches successful/all: [%lu/%lu]", successful_touches,
                 all_touches);
        counter=0;
    }
#endif
    if (!bl) {
#if PRINT_CRITNIB_NOT_FOUND_ON_TOUCH_WARNING
        log_info("WARNING: Addr %p not in known tachanka range  %p - %p", (char*)addr,
            (char*)bl->addr, (char*)(bl->addr + bl->size));
#endif
        assert(from_malloc == 0);
        return;
    }
    if ((char*)addr >= (char*)(bl->addr + bl->size)) {
#if PRINT_CRITNIB_NOT_FOUND_ON_TOUCH_WARNING
        log_info("WARNING: Addr %p not in known tachanka range  %p - %p", (char*)addr,
            (char*)bl->addr, (char*)(bl->addr + bl->size));
#endif
        assert(from_malloc == 0);
        return;
    }

    //printf("bln: %d", bln);
    //printf("bl->type: %d\n", bl->type);

//     else
//     {
//         printf("tachanka touch for known area!\n");
//     }
    assert(bl->type >= 0);
//     assert(bl->type > 0); TODO check if this is the case in our scenario
    struct ttype *t = bl->type;
    // TODO - is this thread safeness needed? or best effort will be enough?
    //__sync_fetch_and_add(&t->accesses, 1);

//     int hotness =1 ;
    size_t total_size = t->total_size;
    if (total_size>0) {
        if (from_malloc) {
            assert(from_malloc == 1); // other case should not occur
            // TODO clean this up, this is (should be?) dead code
            assert(false); // interface changed, this should never be called
//             ranking_add(ranking, bl); // first of all, add
    //         hotness=INIT_MALLOC_HOTNESS; TODO this does not work, for now
        } else {
            // make sth like value * whole size / total size?? this would probably be much better
            // what if new types won't stop appearing?
            // we should adjust for 1. total size of all types and for total number of types
            //
            // init_value * total_all_types_size * number_of_types/total_size_of_current_type
            //
            // this will keep values in range!

            // total_size_all_types: factor that accounts for total allocation
            // size; used in order to avoid making hotness **0**
            size_t total_size_all_types = memtier_kind_get_total_size();
            double hotness =
                HOTNESS_TOUCH_SINGLE_VALUE*total_size_all_types
                /(double)total_size ;
            ranking_touch(ranking, t, timestamp, hotness);
        }
    }

#if PRINT_CRITNIB_TOUCH_INFO
    static atomic_uint_fast16_t counter=0;
    const uint64_t interval=PRINT_POLICY_LOG_STATISTICS_INTERVAL;
    if (++counter > interval) {
        struct timespec t;
        int ret = clock_gettime(CLOCK_MONOTONIC, &t);
        if (ret != 0) {
            log_fatal("ASSERT TOUCH COUNTER FAILURE!\n");
            exit(-1);
        }
        log_info("touch counter %lu hit, [seconds, nanoseconds]: [%ld, %ld]",
            interval, t.tv_sec, t.tv_nsec);
        counter=0u;
    }
#endif

    // TODO make decisions regarding thread-safeness
    // thread-safeness:
    //  - can we actually touch a removed structure?
    //  - is it a problem?
    // current solution: assert(FALSE)
    // future solution: ignore?
//     printf("touches tachanka, timestamp: [%llu]\n", timestamp);
    //assert(g_total_ranking_size == ranking_calculate_total_size(ranking));
}

static bool initialized=false;
void tachanka_init(double old_window_hotness_weight, size_t event_queue_size)
{
#if CHECK_ADDED_SIZE
    // re-initalize global variables
    g_total_critnib_size=0u;
    g_total_ranking_size=0u;
#endif

    int ret = slab_alloc_init(&ttype_alloc, sizeof(struct ttype), 0);
    assert(ret == 0);
    ret = slab_alloc_init(&tblock_alloc, sizeof(struct tblock), 0);
    assert(ret == 0);

    read_maps();

    addr_to_block = critnib_new();
    hash_to_type = critnib_new();

    ranking_create(&ranking, old_window_hotness_weight);
    ranking_event_init(&ranking_event_buff, event_queue_size);

    initialized = true;
}

MEMKIND_EXPORT void tachanka_set_dram_total_ratio(double desired, double actual)
{
    check_dram_total_ratio(desired);
    check_dram_total_ratio(actual);
    g_dramToTotalDesiredRatio = desired;
    g_dramToTotalActualRatio = actual;
}

void tachanka_update_threshold(void)
{
    // where can I take it from ? memkind_memtier! it supports tracking memory for static ratio policy
    ranking_calculate_hot_threshold_dram_total(
        ranking, g_dramToTotalDesiredRatio, g_dramToTotalActualRatio);
}

void tachanka_destroy(void)
{
    initialized = false;
    ranking_destroy(ranking);
    ranking_event_destroy(&ranking_event_buff);

    critnib_delete(addr_to_block);
    critnib_delete(hash_to_type);

    slab_alloc_destroy(&ttype_alloc);
    slab_alloc_destroy(&tblock_alloc);
}

static int _size;
static double _hotness;

static int size_hotness(int nt)
{
    // TODO add API for getting indexed element from slab alloc
    return 0;
}

MEMKIND_EXPORT double tachanka_get_obj_hotness(int size)
{
    // TODO re-add functionality
    _size = size;
    _hotness = -1;
//     critnib_iter(hash_to_type, size_hotness);
    return _hotness;
}

MEMKIND_EXPORT double tachanka_get_addr_hotness(void *addr)
{
    double ret = -1;
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bl) {
        struct ttype *t = bl->type;
        assert(t);
        ret = t->f;
    }
    return ret;
}

// MEMKIND_EXPORT double tachanka_set_touch_callback(void *addr, const char *name)
MEMKIND_EXPORT int tachanka_set_touch_callback(void *addr, tachanka_touch_callback cb, void* arg)
{
    int ret = -1;
    struct tblock *bl = critnib_find_le(addr_to_block, (uintptr_t)addr);
    if (bl) {
        struct ttype *t = bl->type;
        assert(t);
        ranking_set_touch_callback(ranking, cb, arg, t);
        ret=0;
    }
    return ret;
}

MEMKIND_EXPORT bool tachanka_ranking_event_push(EventEntry_t *event)
{
#if OFFLOAD_RANKING_OPS_TO_BACKGROUD_THREAD
    if (initialized == false) {
        log_fatal("push onto non-initialized queue");
        exit(-1);
    }
    bool ret = ranking_event_push(&ranking_event_buff, event);
#if ASSURE_RANKING_DELIVERY
    while (!ret) {
        usleep(1000);
        ret = ranking_event_push(&ranking_event_buff, event);
    }
#endif
#else // EXECUTE SYNCRONOUSLY
    bool ret = true;
    // TODO looks like a good place for refactor - code duplicated with pebs.c
    switch (event->type) {
        case EVENT_CREATE_ADD: {
            EventDataCreateAdd *data = &event->data.createAddData;
            register_block(data->hash, data->address, data->size);
            register_block_in_ranking(data->address, data->size);
            break;
        }
        case EVENT_DESTROY_REMOVE: {
            EventDataDestroyRemove *data = &event->data.destroyRemoveData;
            // REMOVE THE BLOCK FROM RANKING!!!
            // TODO remove all the exclamation marks and clean up once this is done
            unregister_block(data->address);
            break;
        }
        case EVENT_REALLOC: {
            EventDataRealloc *data = &event->data.reallocData;
            unregister_block(data->addressOld);
//                     realloc_block(data->addressOld, data->addressNew, data->sizeNew);
            register_block(0u /* FIXME hash should not be zero !!! */, data->addressNew, data->sizeNew);
            register_block_in_ranking(data->addressNew, data->sizeNew);
            break;
        }
        case EVENT_SET_TOUCH_CALLBACK: {
            EventDataSetTouchCallback *data = &event->data.touchCallbackData;
            tachanka_set_touch_callback(data->address,
                                        data->callback,
                                        data->callbackArg);
            break;
        }
        case EVENT_TOUCH: {
            EventDataTouch *data = &event->data.touchData;
            touch(data->address, data->timestamp, 0 /*called from malloc*/);
            break;
        }
        default: {
            log_fatal("PEBS: event queue - case not implemented!");
            exit(-1);
        }
    }

#endif
    return ret;
}

MEMKIND_EXPORT bool tachanka_ranking_event_pop(EventEntry_t *event)
{
    if (initialized == false) {
        log_fatal("pop from a non-initialized queue");
        exit(-1);
    }
    return ranking_event_pop(&ranking_event_buff, event);
}

MEMKIND_EXPORT void tachanka_ranking_touch_all(__u64 timestamp, double add_hotness)
{
    // TODO re-implement this function
    // add API for getting specific element from slab_allocator
    // and re-implement it
//     for (int i = 0; i < ntypes; ++i) {
//         ranking_touch(ranking, &ttypes[i], timestamp, add_hotness);
//     }
}

// Getter used in the tachanka_check_ranking_touch_all test
MEMKIND_EXPORT double tachanka_get_frequency(size_t index) {
    // TODO: re-implement (add API in slab_allocator first!)
//     return ttypes[index].f;
    return 0;
}

// Getter used in the tachanka_check_ranking_touch_all test
MEMKIND_EXPORT TimestampState_t tachanka_get_timestamp_state(size_t index) {
    // TODO: re-implement (add API in slab_allocator first!)
//     return ttypes[index].timestamp_state;
    return 0;
}

MEMKIND_EXPORT void tachanka_dump_heatmap(void) {
    if (!initialized)
        return;
    heatmap_aggregator_t *aggregator = heatmap_aggregator_create();
    critnib_iter(hash_to_type, 0, -1, aggregate_ttypes, aggregator);
    char *info = heatmap_dump_info(aggregator);
    log_info("heatmap: %s", info);
    heatmap_free_info(info);
    heatmap_aggregator_destroy(aggregator);

}
