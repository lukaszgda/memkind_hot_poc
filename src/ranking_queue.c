#include "ranking_queue.h"

void ranking_event_init(lq_buffer_t *buff, size_t entries) {
    lq_init(buff, sizeof(EventEntry_t), entries);
}

void ranking_event_destroy(lq_buffer_t *buff) {
    lq_destroy(buff);
}

#ifdef USE_MUTEX

#include "pthread.h"

// TODO remove, only for debugging!!!
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

bool ranking_event_push(lq_buffer_t *buff, EventEntry_t *event) {
    pthread_mutex_lock(&mutex);
    bool ret = lq_push(buff, event);
    pthread_mutex_unlock(&mutex);
    return ret;
//     return lq_push(buff, event);
}

bool ranking_event_pop(lq_buffer_t *buff, EventEntry_t *event) {
    pthread_mutex_lock(&mutex);
    bool ret = lq_pop(buff, event);
    pthread_mutex_unlock(&mutex);
    return ret;
//     return lq_pop(buff, event);
}

#else

bool ranking_event_push(lq_buffer_t *buff, EventEntry_t *event) {
    return lq_push(buff, event);
}

bool ranking_event_pop(lq_buffer_t *buff, EventEntry_t *event) {
    return lq_pop(buff, event);
}
#endif
