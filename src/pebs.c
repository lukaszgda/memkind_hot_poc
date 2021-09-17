
#include <memkind/internal/pebs.h>
#include <memkind/internal/tachanka.h>
#include <memkind/internal/critnib.h>
#include <memkind/internal/memkind_private.h>

// smaller value -> more frequent sampling
// 10000 = around 100 samples on *my machine* / sec in matmul test
#define SAMPLE_FREQUENCY 10000
#define MMAP_DATA_SIZE   8
#define rmb() asm volatile("lfence":::"memory")

typedef enum {
    THREAD_INIT,
    THREAD_RUNNING,
    THREAD_FINISHED,
} ThreadState_t;

pthread_t pebs_thread;
ThreadState_t thread_state = THREAD_INIT;
int pebs_fd;
static char *pebs_mmap;

// DEBUG
extern critnib* hash_to_type;
static size_t g_queue_pop_counter=0;
static size_t g_queue_counter_malloc=0;
static size_t g_queue_counter_realloc=0;
static size_t g_queue_counter_callback=0;
static size_t g_queue_counter_free=0;
static size_t g_queue_counter_touch=0;

// static uint64_t timespec_diff_millis(const struct timespec *tnew, const struct timespec *told) {
//     uint64_t diff_s = tnew->tv_sec - told->tv_sec;
//     uint64_t tnew_ns = tnew->tv_nsec;
//     uint64_t told_ns = told->tv_nsec;
//     if (told_ns > tnew_ns) {
//         take 1s and convert it to ns
//         const uint64_t s_to_ns=(uint64_t)1000000000;
//         tnew_ns += s_to_ns;
//         diff_s --;
//     }
//     return diff_s*1000+(tnew_ns-told_ns)/1000000;
// }

static void timespec_add(struct timespec *modified, const struct timespec *toadd) {
    const uint64_t S_TO_NS=1000000000u;
    modified->tv_sec += toadd->tv_sec;
    modified->tv_nsec += toadd->tv_nsec;
    modified->tv_sec += (modified->tv_nsec/S_TO_NS);
    modified->tv_nsec %= S_TO_NS;
}

static void timespec_millis_to_timespec(double millis, struct timespec *out) {
    out->tv_sec = (uint64_t)(millis/1000u);
    double ms_to_convert = millis-out->tv_sec*1000;
    out->tv_nsec = (uint64_t)(ms_to_convert*1000000);
}

static bool timespec_is_he(struct timespec *tv1, struct timespec *tv2) {
    return tv1->tv_sec>tv2->tv_sec || (tv1->tv_sec == tv2->tv_sec && tv1->tv_nsec > tv2->tv_nsec);
}

#define LOG_TO_FILE 1

#include "assert.h"

void *pebs_monitor(void *state)
{
    ThreadState_t* pthread_state = state;

//     double freq_Hz=1;
    double freq_Hz=100;
    double period_ms=1000/freq_Hz;
    struct timespec tv_period;
    timespec_millis_to_timespec(period_ms, &tv_period);
    // set low priority
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    param.sched_priority = sched_get_priority_min(policy);
    pthread_setschedparam(pthread_self(), policy, &param);

    __u64 last_head = 0;

    // DEBUG
    char buf[4096];
#if LOG_TO_FILE
    static int pid;
    static int log_file;
    int cur_pid = getpid();
    int cur_tid = gettid();
    printf("starting pebs monitor for thread %d\n", cur_tid);
    if (pid != cur_pid) {
        char name[255] = {0};
        sprintf(name, "tier_pid_%d_tid_%d.log", cur_pid, cur_tid);
        log_file = open(name, O_CREAT | O_WRONLY, S_IRWXU);
    }
    lseek(log_file, 0, SEEK_END);
#endif

    struct timespec ntime;
    int ret = clock_gettime(CLOCK_MONOTONIC, &ntime);
    if (ret != 0) {
        printf("ASSERT_CLOCK_GETTIME_FAILURE!\n");
        assert(ret!=0);
    }
    timespec_add(&ntime, &tv_period);


    while (1) {
        // TODO - use mutex?
        if (*pthread_state == THREAD_FINISHED) {
            return NULL;
        }

        // must call this before read from data head
		rmb();

        {
            static uint64_t counter=0;
            const uint64_t interval=1000;
            if (++counter > interval) {
                struct timespec t;
                int ret = clock_gettime(CLOCK_MONOTONIC, &t);
                if (ret != 0) {
                    printf("ASSERT PEBS COUNTER FAILURE!\n");
                }
                assert(ret == 0);
                printf("pebs counter %lu hit, succcessful ranking_queue reads %lu, time [seconds, nanoseconds]: [%ld, %ld]\n",
                    interval, g_queue_pop_counter, t.tv_sec, t.tv_nsec);
// g_queue_counter_malloc, g_queue_counter_realloc, g_queue_counter_callback, g_queue_counter_free, g_queue_counter_touch

                printf("g_queue_counter_malloc: %lu, g_queue_counter_realloc: %lu, g_queue_counter_callback: %lu, g_queue_counter_free: %lu, g_queue_counter_touch: %lu\n", g_queue_counter_malloc, g_queue_counter_realloc, g_queue_counter_callback, g_queue_counter_free, g_queue_counter_touch);
                counter=0u;
            }
        }
        struct perf_event_mmap_page* pebs_metadata =
            (struct perf_event_mmap_page*)pebs_mmap;

        // DEBUG
        // printf("head: %llu size: %lld\n", pebs_metadata->data_head, pebs_metadata->data_head - last_head);
        if (last_head < pebs_metadata->data_head) {
//             printf("new data from PEBS\n");
            int samples = 0;

            while (last_head < pebs_metadata->data_head) {
	            char *data_mmap = pebs_mmap + getpagesize() +
                    last_head % (MMAP_DATA_SIZE * getpagesize());

                struct perf_event_header *event =
                    (struct perf_event_header *)data_mmap;

//  {    static uint64_t counter=0;
//     const uint64_t interval=10;
//     if (++counter > interval) {
//         struct timespec t;
//         int ret = clock_gettime(CLOCK_MONOTONIC, &t);
//         if (ret != 0) {
//             printf("ASSERT PEBS EVENT COUNTER FAILURE!\n");
//         }
//         assert(ret == 0);
//         printf("pebs event counter 10 hit, [seconds, nanoseconds]: [%ld, %ld]\n",
//             t.tv_sec, t.tv_nsec);
//         counter=0u;
//     }
// }
                switch (event->type) {
                    case PERF_RECORD_SAMPLE:
                    {
                        __u64 timestamp = *(__u64*)(data_mmap + sizeof(struct perf_event_header));
                        // 'addr' is the acessed address
                        __u64 addr = *(__u64*)(data_mmap + sizeof(struct perf_event_header) + sizeof(__u64));

                        // TODO - is this a global or per-core timestamp? If per-core, this could lead to some problems

// {    static uint64_t counter=0;
//     const uint64_t interval=10;
//     if (++counter > interval) {
//         struct timespec t;
//         int ret = clock_gettime(CLOCK_MONOTONIC, &t);
//         if (ret != 0) {
//             printf("ASSERT PEBS TOUCH COUNTER FAILURE!\n");
//         }
//         assert(ret == 0);
//         printf("pebs touch counter 10 hit, [seconds, nanoseconds]: [%ld, %ld]\n",
//             t.tv_sec, t.tv_nsec);
//         counter=0u;
//     }
// }
//                         printf("touches, timestamp: [%llu], from malloc [0]\n", timestamp);
                        touch((void*)addr, timestamp, 0 /* from malloc */);
//                         printf("touched, timestamp: [%llu], from malloc [0]\n", timestamp);
#if LOG_TO_FILE
                        // DEBUG
                        sprintf(buf, "last: %llu, head: %llu t: %llu addr: %llx\n",
                            last_head, pebs_metadata->data_head,
                            timestamp, addr);
                        //if (write(log_file, buf, strlen(buf))) ;
#endif
                        //printf("%s", buf);
                    }
                    break;
                default:
                    break;
                };

                last_head += event->size;
                data_mmap += event->size;
                samples++;
            }

            printf("new data from pebs: %d samples\n", samples);

#if LOG_TO_FILE
            // DEBUG
            int nchars = 0;
            int total_chars = 0;
            for (int i = 0; i < 20; i++) {
                struct ttype* tb = critnib_get_leaf(hash_to_type, i);

                if (tb != NULL && tb->timestamp_state == TIMESTAMP_INIT_DONE)
                {
                    float f = tb->f;
                    sprintf(buf + total_chars, "%f,%n", f, &nchars);
                }
                else {
                    sprintf(buf + total_chars, "N/A,%n", &nchars);
                }

                total_chars += nchars;
            }
            sprintf(buf + total_chars, "\n");
            if (write(log_file, buf, strlen(buf)));
#endif
        }

		ioctl(pebs_fd, PERF_EVENT_IOC_REFRESH, 0);
        last_head = pebs_metadata->data_head;
        pebs_metadata->data_tail = pebs_metadata->data_head;

        EventEntry_t event;
        bool pop_success;
        while (true) {
            pop_success = tachanka_ranking_event_pop(&event);
            if (!pop_success)
                break;
            switch (event.type) {
                case EVENT_CREATE_ADD:
                    register_block(event.data.createAddData.hash, event.data.createAddData.address, event.data.createAddData.size);
                    touch(event.data.createAddData.address, 0, 1 /*called from malloc*/);
                    g_queue_counter_malloc++;
                    break;
                case EVENT_DESTROY_REMOVE:
                    unregister_block(event.data.destroyRemoveData.address);
                    g_queue_counter_free++;
                    break;
                case EVENT_REALLOC:
                    unregister_block(event.data.reallocData.address);
                    g_queue_counter_realloc++;
                    break;
                case EVENT_SET_TOUCH_CALLBACK:
                    tachanka_set_touch_callback(
                        event.data.touchCallbackData.address,
                        event.data.touchCallbackData.callback,
                        event.data.touchCallbackData.callbackArg);
                    g_queue_counter_callback++;
                    break;
                case EVENT_TOUCH:
                    assert(false && "not implemented here!");
                    g_queue_counter_touch++;
                    break;
                default:
                    assert(false && "case not implemented!");

            }
            g_queue_pop_counter++;
        }

        tachanka_update_threshold();
//         ret = clock_gettime(CLOCK_MONOTONIC, &ctime);
//         if (ret != 0) {
//             printf("ASSERT_CLOCK_GETTIME_FAILURE!\n");
//             assert(ret!=0);
//         }
//         uint64_t diff_ms = timespec_diff_millis(&ctime, &ptime);
//         if ()
//
//         usleep(1000*(diff_ms-period_ms))
//         if (diff_ms > period_ms) {
//             warning:
//         }
//         ptime = ctime;

// 		sleep(1); // TODO analysis depends on event number; better solution: const 1 Hz, with low priority
//         usleep(1000);
        struct timespec temp;
        ret = clock_gettime(CLOCK_MONOTONIC, &temp);
        if (ret != 0) {
            printf("ASSERT_CLOCK_GETTIME_FAILURE!\n");
            assert(ret!=0);
        }
        if (timespec_is_he(&temp, &ntime)) {
            printf("WARN: deadline not met!");
        }
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ntime, NULL);
        timespec_add(&ntime, &tv_period);
    }
    printf("stopping pebs monitor for thread %d", cur_tid);

    return NULL;
}

void pebs_init(pid_t pid)
{
    // TODO add code that writes to /proc/sys/kernel/perf_event_paranoid ?

    printf("pebs_init\n");
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));

    // NOTE: code bellow requires link to libpfm
    int ret = pfm_initialize();
    if (ret != PFM_SUCCESS) {
        printf("pfm_initialize() failed!\n");
        exit(-1);
    }

    pfm_perf_encode_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.attr = &pe;

    char event[] = "MEM_LOAD_RETIRED:L3_MISS";
    //char event[] = "MEM_UOPS_RETIRED:ALL_LOADS";

    ret = pfm_get_os_event_encoding(event, PFM_PLM3, PFM_OS_PERF_EVENT_EXT, &arg);
    if (ret != PFM_SUCCESS) {
        //printf("pfm_get_os_event_encoding() failed!\n");
        //exit(-1);

        pe.type = 4;
        pe.config = 0x5120D1;
    }

    pe.size = sizeof(struct perf_event_attr);
    pe.sample_period = SAMPLE_FREQUENCY;
    pe.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_TIME;

    pe.precise_ip = 2; // NOTE: this is reqired but was not set
                       // by pfm_get_os_event_encoding()
    pe.read_format = 0;
    pe.disabled = 1;
    pe.pinned = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.wakeup_events = 1;

    //pid_t pid = 0;              // measure current process
    int cpu = -1;               // .. on any CPU
    int group_fd = -1;          // use single event group
    unsigned long flags = 0;
    pebs_fd = perf_event_open(&pe, pid, cpu, group_fd, flags);

    if (pebs_fd != -1) {
        int mmap_pages = 1 + MMAP_DATA_SIZE;
        int map_size = mmap_pages * getpagesize();
        pebs_mmap = mmap(NULL, map_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, pebs_fd, 0);

        // DEBUG
        //printf("PEBS thread start\n");

        thread_state = THREAD_RUNNING;
        pthread_create(&pebs_thread, NULL, &pebs_monitor, (void*)&thread_state);

        ioctl(pebs_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(pebs_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    else
    {
        printf("PEBS NOT SUPPORTED! continuing without pebs\n");
    }
}

void pebs_fini()
{
    // finish only if the thread is running
    if (thread_state == THREAD_RUNNING) {
        ioctl(pebs_fd, PERF_EVENT_IOC_DISABLE, 0);

        // TODO - use mutex?
        thread_state = THREAD_FINISHED;
        void* ret;
        pthread_join(pebs_thread, &ret);

        int mmap_pages = 1 + MMAP_DATA_SIZE;
        munmap(pebs_mmap, mmap_pages * getpagesize());
        pebs_mmap = 0;
        close(pebs_fd);

        // DEBUG
        //printf("PEBS thread end\n");
    }
}

MEMKIND_EXPORT void pebs_fork(pid_t pid)
{
    pebs_init(pid);
}
