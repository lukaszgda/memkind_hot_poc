#pragma once

#include "lockless_srmw_queue.h"

// uncomment to use mutex - don't rely on correctness of
// lockless structure implementation
// #define USE_MUTEX

typedef enum EventType {
    EVENT_CREATE_ADD,
    EVENT_TOUCH,
} EventType_t;


typedef union EventData {
    uint64_t hash;
    uint64_t address;
} EventData_t;

typedef struct EventEntry {
    EventType_t type;
    EventData_t data;
} EventEntry_t;

extern void ranking_event_init(lq_buffer_t *buff, size_t entries);

extern void ranking_event_destroy(lq_buffer_t *buff);

extern bool ranking_event_push(lq_buffer_t *buff, EventEntry_t *event);

extern bool ranking_event_pop(lq_buffer_t *buff, EventEntry_t *event);
