#include "assert.h"
#include "jemalloc/jemalloc.h"
#include "memkind/internal/ranking_queue.h"
#include "stdlib.h"
#include "string.h"

// command to compile tests:
// gcc lockless_srmw_queue.c ranking_queue.c ranking_queue_tests.c -march=native
// -pthread -O3
// TODO move to gtest

static void test_simple(void)
{
    //
    lq_buffer_t *buff;
    ranking_event_create(&buff, 4);
    EventEntry_t entry;
    bool empty_poppable = ranking_event_pop(buff, &entry);
    assert(!empty_poppable);

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 1u;
    bool added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)2u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)3u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 4u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 5u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");

    bool popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 1u);

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)2u);

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)3u);

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 4u);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    // queue empty, refill

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)6u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)7u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_CREATE_ADD;
    entry.data.touchData.address = (void *)8u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 9u;
    added = ranking_event_push(buff, &entry);
    assert(added);

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 10u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 10u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 10u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)6u);

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)7u);

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 8u);

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 9u);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    ranking_event_destroy(buff);
}

static void test_simple_refill(void)
{
    //
    lq_buffer_t *buff;
    ranking_event_create(&buff, 4);
    EventEntry_t entry;
    bool empty_poppable = ranking_event_pop(buff, &entry);
    assert(!empty_poppable);

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 1u;
    bool added = ranking_event_push(buff, &entry);
    assert(added);
    // 1 on queue, 3 empty

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)2u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 2 on queue, 2 empty

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)3u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 3 on queue, 1 empty

    bool popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 1u);
    // 2 on queue, 2 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)2u);
    // 1 on queue, 3 empty

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 4u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 2 on queue, 2 empty

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)6u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 3 on queue, 1 empty

    entry.type = EVENT_TOUCH;
    entry.data.touchData.address = (void *)7u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 4 on queue, 0 empty

    // queue full
    entry.type = EVENT_CREATE_ADD;
    entry.data.touchData.address = (void *)8u;
    added = ranking_event_push(buff, &entry);
    assert(!added);
    // 4 on queue, 0 empty

    // queue full
    entry.type = EVENT_CREATE_ADD;
    entry.data.touchData.address = (void *)8u;
    added = ranking_event_push(buff, &entry);
    assert(!added);
    // 4 on queue, 0 empty

    // queue full
    entry.type = EVENT_CREATE_ADD;
    entry.data.touchData.address = (void *)8u;
    added = ranking_event_push(buff, &entry);
    assert(!added);
    // 4 on queue, 0 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.createAddData.hash == 3u);
    // 3 on queue, 1 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 4u);
    // 2 on queue, 2 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)6u);
    // 1 on queue, 3 empty

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 9u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 2 on queue, 2 empty

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 10u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 3 on queue, 1 empty

    entry.type = EVENT_CREATE_ADD;
    entry.data.createAddData.hash = 11u;
    added = ranking_event_push(buff, &entry);
    assert(added);
    // 4 on queue, 0 empty

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 12u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");
    // 4 on queue, 0 empty

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 12u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");
    // 4 on queue, 0 empty

    entry.type = EVENT_TOUCH;
    entry.data.createAddData.hash = 12u;
    added = ranking_event_push(buff, &entry);
    assert(!added && "queue full!");
    // 4 on queue, 0 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_TOUCH);
    assert(entry.data.touchData.address == (void *)7u);
    // 3 on queue, 1 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 9u);
    // 2 on queue, 2 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 10u);
    // 1 on queue, 3 empty

    popped = ranking_event_pop(buff, &entry);
    assert(popped);
    assert(entry.type == EVENT_CREATE_ADD);
    assert(entry.data.createAddData.hash == 11u);
    // 0 on queue, 4 empty

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    popped = ranking_event_pop(buff, &entry);
    assert(!popped);

    ranking_event_destroy(buff);
}

typedef struct TestDataWriter {
    EventEntry_t *entries;
    size_t entriesSize;
    lq_buffer_t *buff;
} TestDataWriter;

typedef struct TestDataReader {
    EventEntry_t *dest;
    size_t destSize;
    lq_buffer_t *buff;
} TestDataReader;

static void *write_batch(void *data)
{
    TestDataWriter *data_ = data;
    // TODO
    for (int i = 0; i < data_->entriesSize; ++i) {
        while (!ranking_event_push(data_->buff, &data_->entries[i]))
            ;
    }

    return NULL;
}

static void *read_batch(void *data)
{
    TestDataReader *data_ = data;
    for (size_t i = 0; i < data_->destSize; ++i) {
        // TODO
        EventEntry_t temp;
        while (!ranking_event_pop(data_->buff, &temp))
            ;
        assert(temp.type == EVENT_TOUCH);
        assert(((size_t)temp.data.touchData.address) < data_->destSize);
        data_->dest[(size_t)temp.data.touchData.address] = temp;
    }
    return NULL;
}

#include "pthread.h"

static void stress_test_simple(size_t writers, size_t params_per_thread,
                               size_t buffer_size, size_t iterations)
{
    // scenario:
    //  - create source array,
    //  - distribute source array chunks between writers,
    //  - write and read simultaneously (all writers, one reader),
    //  - check that all elements were correctly read

    size_t source_size = writers * params_per_thread;
    EventEntry_t *entries_source =
        jemk_malloc(source_size * sizeof(EventEntry_t));
    EventEntry_t *entries_dest = jemk_calloc(source_size, sizeof(EventEntry_t));
    // TODO init entries source and dest
    for (size_t i = 0; i < source_size; ++i) {
        entries_source[i].type = EVENT_TOUCH;
        entries_source[i].data.touchData.address = (void *)i;
    }
    lq_buffer_t *buff;
    ranking_event_create(&buff, buffer_size);
    TestDataReader reader_data = {
        .dest = entries_dest,
        .destSize = source_size,
        .buff = buff,
    };
    TestDataWriter *writers_data =
        jemk_malloc(writers * sizeof(TestDataWriter));
    for (size_t i = 0; i < writers; ++i) {
        writers_data[i].entries = &entries_source[i * params_per_thread];
        writers_data[i].entriesSize = params_per_thread;
        writers_data[i].buff = buff;
    }
    // perform the whole test here!
    // create reader
    for (size_t it = 0; it < iterations; ++it) {
        int ret;
        pthread_t treader;
        pthread_t twriters[writers];
        ret = pthread_create(&treader, NULL, &read_batch, &reader_data);
        assert(ret == 0);
        // create writers
        for (size_t i = 0; i < writers; ++i) {
            ret = pthread_create(&twriters[i], NULL, &write_batch,
                                 &writers_data[i]);
            assert(ret == 0);
        }
        // join writers
        for (size_t i = 0; i < writers; ++i) {
            ret = pthread_join(twriters[i], NULL);
            assert(ret == 0);
        }
        // join reader
        ret = pthread_join(treader, NULL);
        assert(ret == 0);

        // check dest
        for (size_t i = 0; i < source_size; ++i) {
            // check ith dest
            assert(entries_dest[i].type == EVENT_TOUCH);
            assert(entries_dest[i].data.touchData.address == (void *)i);
        }
        // clear dest
        memset(entries_dest, source_size, sizeof(EventEntry_t));
    }
    jemk_free(writers_data);
    ranking_event_destroy(buff);
    jemk_free(entries_dest);
    jemk_free(entries_source);
}

static void stress_tests_simple(void)
{
    stress_test_simple(1, 10000000, 10000000, 20);
    //     stress_test_simple(10, 1000000, 100000);
    stress_test_simple(10, 1000000, 1000000, 1);
    //     stress_test_simple(3, 1000000, 100000);
    //     stress_test_simple(2, 5000000, 10000000, 1);
}

int main()
{
    test_simple();
    test_simple_refill();
    stress_tests_simple();
    return 0;
}
