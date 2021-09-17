#include "memkind/internal/ranking_queue.h"
#include "memkind/internal/lockless_srmw_queue.h"
#include "jemalloc/jemalloc.h"

#ifndef MEMKIND_EXPORT
#define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

MEMKIND_EXPORT void ranking_event_create(lq_buffer_t **buff, size_t entries) {
    *buff = jemk_malloc(sizeof(lq_buffer_t));
    ranking_event_init(*buff, entries);
}

MEMKIND_EXPORT void ranking_event_destroy(lq_buffer_t *buff) {
    ranking_event_fini(buff);
    jemk_free(buff);
}

MEMKIND_EXPORT void ranking_event_init(lq_buffer_t *buff, size_t entries) {
    lq_init(buff, sizeof(EventEntry_t), entries);
}

MEMKIND_EXPORT void ranking_event_fini(lq_buffer_t *buff) {
    lq_destroy(buff);
}

#ifdef USE_MUTEX

#include "pthread.h"

// TODO remove, only for debugging!!!
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

MEMKIND_EXPORT bool ranking_event_push(lq_buffer_t *buff, EventEntry_t *event) {
    pthread_mutex_lock(&mutex);
    bool ret = lq_push(buff, event);
    pthread_mutex_unlock(&mutex);
    return ret;
//     return lq_push(buff, event);
}

MEMKIND_EXPORT bool ranking_event_pop(lq_buffer_t *buff, EventEntry_t *event) {
    pthread_mutex_lock(&mutex);
    bool ret = lq_pop(buff, event);
    pthread_mutex_unlock(&mutex);
    return ret;
//     return lq_pop(buff, event);
}

#else

MEMKIND_EXPORT bool ranking_event_push(lq_buffer_t *buff, EventEntry_t *event) {
    return lq_push(buff, event);
}

MEMKIND_EXPORT bool ranking_event_pop(lq_buffer_t *buff, EventEntry_t *event) {
    return lq_pop(buff, event);
}
#endif
