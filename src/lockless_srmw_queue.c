#include "stdlib.h"
#include "assert.h"
#include "string.h"
#include "lockless_srmw_queue.h"

// TODO PLEASE NOTE THAT CURRENT LOCKLESS VERSION IS WIP AND IS NOT FULLY IMPLEMENTED!!!!!
// #define atomic_thread_fence(x)     (void)(x)

// srmw: single reader, multiple writers
// TODO TODO check fences and memory order - probably some atomic operations
// don't require sequentially-consistent ordering!
// TODO make sure the readers won't overflow buffer? only one reader!

// ---- generic lockless queue (srmw: single reader, multiple writers)

/// # Intro:
///
/// The document covers design principles of a queue used for offloading
/// asynchronous, callback-less computations from critical path to
/// a separate thread.
///
/// # Design requirements:
///
///     1. Lockless (no wait on sync and no syscalls during write)
///     2. Single reader, multiple writers
///     3. Reader can poll for values at specific intervals
///     4. Write overhead should be minimal - subject to optimisation
///     5. Write-read latency is not critical
///     6. No external allocators are required
///     7. Simple, usable wrapper can be created later
///
/// # High-level overview:
///
///     1. Internal ring buffer
///     2. All allocations performed at init
///     3. Readers can request for place at internal buffer
///     4. When writing is finished, the requested entry should be posted
///
/// ## Sample workflow - writer:
///
///     lq_buffer_t buff;
///     // don't duplicate between readers/writers !
///     size_t entry_size = ...;
///     size_t entries = ...;
///     lq_init(&buff, entry_size, entries);
///     // eof don't duplicate
///     lq_entry_t *entry = lq_request_entry_read(&buff);
///     if (entry) {
///         // entry->data contains allocated memory
///         // of the size specified at init entry_size
///         // it can be accessed and written to until lq_post_entry
///         // for this specific element is called
///         write_data(entry);      // to be implemented by user
///         lq_post_entry_write(entry);   // data is available for read
///     }
///
/// ## Sample workflow - reader:
///
///     lq_buffer_t buff;
///     // don't duplicate between readers/writers !
///     size_t entry_size = ...;
///     size_t entries = ...;
///     lq_init(&buff, entry_size, entries);
///     // eof don't duplicate
///     while (true) {
///         lq_entry_t *entry = lq_request_entry_read(&buff);
///         // data is removed from the queue
///         // entry->data contains allocated memory
///         // of the size specified at init entry_size
///         // it can be accessed and read from until lq_post_entry
///         // for this specific element is called
///         if (entry) {
///             handle_data(entry);
///             lq_post_entry_read(entry); // entry is now available for write
///         }
///     }
///
/// # General internals:
///
///     - internal data structure: ring buffer:
///         a) head and tail correspond to "used" entries
///         b) entry is considered "used" between:
///             1) lq_request_entry_write,
///             2) corresponding lq_post_entry_read,
///         c) each entry contains metadata - state, one of:
///             1) ENTRY_FREE: entry can be requested for write,
///             2) ENTRY_UNDER_WRITING: entry was requested for write,
///             3) ENTRY_READY: entry can be requested for read,
///             4) ENTRY_UNDER_READING: entry was requested for read
///         d) handling of each state:
///     - memory barriers are required at each metadata change:
///         a) neccessary "happens before" relationships:
///             1) lq_request_entry_write -> writing data
///             2) writing data -> lq_post_entry_write
///             3) lq_post_entry_write -> lq_request_entry_read // ok
///             4) lq_request_entry_read -> read data
///             5) read data -> post_entry_read
///             6) lq_post_entry_read -> lq_request_entry_write
///         b) how to implement? generic atomics? explicit memory barriers?
///
/// # Corner cases and how they are handled:
///
///     - lq_post_entry_XXX is not called:
///         a) corresponding lq_request_entry_read fails,
///         b) corresponding lq_request_entry_write fails,
///         c) such case should be avoided
///
/// # Simple wrapper API that uses implicit copy:
///
///     void *data = ...;                   // has to have specific size
///     lq_push(&buff, data)                // implicit copy
///     void *popped_data = lq_pop(&buff);  // implicit copy
///     handle_data(popped_data);           // to be implemented by user

#define META_STATE_FREE             0u
#define META_STATE_UNDER_WRITING    1u
#define META_STATE_READY            2u
#define META_STATE_UNDER_READING    3u

static void lq_cancel_reserve_write(lq_buffer_t *buff) {
    buff->used--;
}

// TODO potential refactor - only one lq_reserve, with an additional argument!
static bool lq_reserve_write(lq_buffer_t *buff) {
    // TODO check explicit memory order!
    size_t prev_size = atomic_fetch_add(&buff->used, 1);
    if (prev_size < buff->size) {
        // enough place on buffer, operations can continue
        return true;
    } else {
        // not enough place on buffer, failure
        lq_cancel_reserve_write(buff);

        return false;
    }
}

static void lq_cancel_reserve_read(lq_buffer_t *buff) {
    buff->unavailableRead--;
}

// TODO potential refactor - only one lq_reserve, with an additional argument!
static bool lq_reserve_read(lq_buffer_t *buff) {
    // TODO check explicit memory order!
    size_t prev_size = atomic_fetch_add(&buff->unavailableRead, 1);
    if (prev_size < buff->size) {
        // enough place on buffer, operations can continue
        // elements are added at tail and taken
        return true;
    } else {
        // not enough place on buffer, failure
        lq_cancel_reserve_read(buff);
        return false;
    }
}

// TODO
static lq_entry_t *lq_request_entry_read(lq_buffer_t *buff) {
    // TODO only single reader scenario!
    lq_entry_t *ret = NULL;
    if (lq_reserve_read(buff)) {
        // this is a single reader,
        // in theory - sync below is not necessary
        size_t head_idx_old, head_idx_new;
        do {
            head_idx_old = buff->head;
            head_idx_new = head_idx_old;
            head_idx_new++;
            head_idx_new%=buff->size;
        } while (!atomic_compare_exchange_weak(&buff->head, &head_idx_old, head_idx_new));
        ret = &buff->entries[head_idx_old];
        // load all non-atomic writes that were released on other threads
        atomic_thread_fence(memory_order_acquire);
//         atomic_thread_fence(memory_order_seq_cst);
        if (ret->metadata_state != META_STATE_READY) {
            // atomic operation not necessary - single reader scenario
            // writes were done out of order, perform rollback!
            buff->head = head_idx_old; // rollback
            lq_cancel_reserve_read(buff);
            // case where 2 reads happened out of order
            // first one was not ok, so a rollback begun
            // second case was ok, so rollback did not happen
            // the second case won't happen in a single-reader scenario!!!!
            // only one read at a time
            ret = NULL;
        }
    }

    return ret;
}


// TODO remove comment, this one is ok
static lq_entry_t *lq_request_entry_write(lq_buffer_t *buff) {
    lq_entry_t *ret = NULL;
    if (lq_reserve_write(buff)) {
        size_t tail_idx_old, tail_idx_new;
        do {
            tail_idx_old = buff->tail;
            tail_idx_new = tail_idx_old;
            tail_idx_new++;
            tail_idx_new%=buff->size;
        } while (!atomic_compare_exchange_weak(&buff->tail, &tail_idx_old, tail_idx_new));
        ret = &buff->entries[tail_idx_old];
        // load all non-atomic writes that were released on other threads
        atomic_thread_fence(memory_order_acquire);
//         atomic_thread_fence(memory_order_seq_cst);
        if (ret->metadata_state != META_STATE_FREE) {
            // reads were done out of order, perform rollback!
            // for now, impossible - single reader scenario!
            lq_cancel_reserve_write(buff);
            assert(false && "read out of order! Is there >1 reader?");
        }
    }

    return ret;
}

// TODO review states - under reading and under wrinting are unnecessary/irrelevant?

static void lq_post_entry_write(lq_buffer_t *buff, lq_entry_t *entry) {
    // finalize all non-atomic writes on this thread
    atomic_thread_fence(memory_order_release);
//     atomic_thread_fence(memory_order_seq_cst);
    entry->metadata_state=META_STATE_READY;
    atomic_thread_fence(memory_order_release);
//     atomic_thread_fence(memory_order_seq_cst);
    // no reorder! Correct order:
    //  1) all non-atomic reads
    //  2) metadata_state
    //  3) unavailable read
    buff->unavailableRead--; // mark that there is an element available for read
}

static void lq_post_entry_read(lq_buffer_t *buff, lq_entry_t *entry) {
    // finalize all non-atomic writes on this thread
//     atomic_thread_fence(memory_order_release);
//     atomic_thread_fence(memory_order_seq_cst);
    entry->metadata_state=META_STATE_FREE;
    // only one reader, only one fence necessary
    atomic_thread_fence(memory_order_release);
//     atomic_thread_fence(memory_order_seq_cst);
    // TODO no reorder !
//     buff->usedEnd--;
    buff->used--;
}

// two separate buffers, or one big?
void lq_init(lq_buffer_t *buff, size_t entry_size, size_t entries) {
    buff->size = entries;
    buff->entrySize = entry_size;
    buff->tail = 0u;
    buff->head = 0u;
    buff->used = 0u;
    buff->unavailableRead = entries;
    // TODO don't use malloc, use jemk malloc, or sth
    buff->entries = malloc(entries*sizeof(lq_entry_t));
    buff->data = malloc(entry_size * entries);
    for (size_t i=0; i< entries; ++i) {
        // initialize each entry
        buff->entries[i].metadata_state = META_STATE_FREE;
        // TODO check alignment
        buff->entries[i].data = &((uint8_t*)buff->data)[i*entry_size];
    }
}

void lq_destroy(lq_buffer_t *buff) {
    free(buff->entries);
    free(buff->data);
}

bool lq_pop(lq_buffer_t *buff, void *out) {
    bool ret = false;
    lq_entry_t *temp = lq_request_entry_read(buff);
    if (temp) {
        ret=true;
        memcpy(out, temp->data, buff->entrySize);
        lq_post_entry_read(buff, temp);
    }
    return ret;
}

bool lq_push(lq_buffer_t *buff, void *in) {
    bool ret = false;
    lq_entry_t *temp = lq_request_entry_write(buff);
    if (temp) {
        ret=true;
        memcpy(temp->data, in, buff->entrySize);
        lq_post_entry_write(buff, temp);
    }
    return ret;
}
