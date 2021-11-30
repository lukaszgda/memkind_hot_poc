#pragma once

// #include "lockless_srmw_queue.h"
#include "asm-generic/int-ll64.h"
#include "stdbool.h"
#include "stdint.h"
#include "stdlib.h"
// uncomment to use mutex - don't rely on correctness of
// lockless structure implementation
// #define USE_MUTEX

typedef struct lq_buffer lq_buffer_t;

typedef enum EventType
{
    EVENT_CREATE_ADD,
    EVENT_DESTROY_REMOVE,
    EVENT_REALLOC,
    EVENT_TOUCH,
    EVENT_SET_TOUCH_CALLBACK,
} EventType_t;

typedef struct EventDataTouch {
    void *address;
    __u64 timestamp;
} EventDataTouch;

typedef struct EventDataDestroyRemove {
    void *address;
    size_t size;
} EventDataDestroyRemove;

typedef struct EventDataRealloc {
    void *addressOld;
    void *addressNew;
    size_t sizeOld;
    size_t sizeNew;
    bool isHot;
} EventDataRealloc;

typedef struct EventDataCreateAdd {
    uint64_t hash;
    void *address;
    size_t size;
    bool isHot;
    // TODO use size in TOUCH!!!
    // idea: pass it instead of FROM_MALLOC parameter:
    // nonzero-> from_malloc, size; zero: pebs touch, not from malloc!
} EventDataCreateAdd;

typedef struct EventDataSetTouchCallback {
    void *address;
    void (*callback)(void *);
    void *callbackArg;
    ;
} EventDataSetTouchCallback;

typedef union EventData {
    EventDataTouch touchData;
    EventDataCreateAdd createAddData;
    EventDataDestroyRemove destroyRemoveData;
    EventDataRealloc reallocData;
    EventDataSetTouchCallback touchCallbackData;
} EventData_t;

typedef struct EventEntry {
    EventType_t type;
    EventData_t data;
} EventEntry_t;

// API 1: create-destroy

/// @brief allocate and initialize @p buff
/// @note ranking_event_init and ranking_event_fini should not be called!
/// @note please call ranking_event_destroy to free allocated memory
extern void ranking_event_create(lq_buffer_t **buff, size_t entries);

extern void ranking_event_destroy(lq_buffer_t *buff);

/// @brief initialize @p buff
/// @note ranking_event_create and ranking_event_destroy should not be called!
/// @note please call ranking_event_fini to free allocated memory
extern void ranking_event_init(lq_buffer_t *buff, size_t entries);

extern void ranking_event_fini(lq_buffer_t *buff);

extern bool ranking_event_push(lq_buffer_t *buff, EventEntry_t *event);

extern bool ranking_event_pop(lq_buffer_t *buff, EventEntry_t *event);
