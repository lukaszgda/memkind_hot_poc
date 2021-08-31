
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
extern struct ttype ttypes[];
static char *bp;
static int display_hotness(int nt)
{
    const struct ttype *t = &ttypes[nt];
    if (t->timestamp_state == TIMESTAMP_INIT_DONE)
        bp += sprintf(bp, "%f,", t->f);
    else
        bp += sprintf(bp, "N/A,");
    return 0;
}

#define LOG_TO_FILE 1

void *pebs_monitor(void *state)
{
    ThreadState_t* pthread_state = state;

    // set low priority
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    param.sched_priority = sched_get_priority_min(policy);
    pthread_setschedparam(pthread_self(), policy, &param);

    __u64 last_head = 0;

    // DEBUG
#if LOG_TO_FILE
    char buf[4*1048576];
    static int pid;
    static int log_file;
    int cur_pid = getpid();
    int cur_tid = syscall(SYS_gettid);
    printf("starting pebs monitor for thread %d", cur_tid);
    if (pid != cur_pid) {
        char name[255] = {0};
        sprintf(name, "tier_pid_%d_tid_%d.log", cur_pid, cur_tid);
        log_file = open(name, O_CREAT | O_WRONLY, S_IRWXU);
    }
    lseek(log_file, 0, SEEK_END);
#endif

    while (1) {
        // TODO - use mutex?
        if (*pthread_state == THREAD_FINISHED) {
            return NULL;
        }

        // must call this before read from data head
		rmb();

        struct perf_event_mmap_page* pebs_metadata =
            (struct perf_event_mmap_page*)pebs_mmap;

        // DEBUG
        // printf("head: %llu size: %lld\n", pebs_metadata->data_head, pebs_metadata->data_head - last_head);
        if (last_head < pebs_metadata->data_head) {
            printf("new data from PEBS: ");
            int samples = 0;

            while (last_head < pebs_metadata->data_head) {
	            char *data_mmap = pebs_mmap + getpagesize() +
                    last_head % (MMAP_DATA_SIZE * getpagesize());

                struct perf_event_header *event =
                    (struct perf_event_header *)data_mmap;

                switch (event->type) {
                    case PERF_RECORD_SAMPLE:
                    {
                        __u64 timestamp = *(__u64*)(data_mmap + sizeof(struct perf_event_header));
                        // 'addr' is the acessed address
                        __u64 addr = *(__u64*)(data_mmap + sizeof(struct perf_event_header) + sizeof(__u64));

                        // TODO - is this a global or per-core timestamp? If per-core, this could lead to some problems

                        touch((void*)addr, timestamp, 0 /* from malloc */);
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

            printf("%d samples\n", samples);

            bp = buf;
            critnib_iter(hash_to_type, display_hotness);
            bp += sprintf(bp, "\n");
            if (write(log_file, buf, bp - buf));
        }

		ioctl(pebs_fd, PERF_EVENT_IOC_REFRESH, 0);
        last_head = pebs_metadata->data_head;
        pebs_metadata->data_tail = pebs_metadata->data_head;

        tachanka_update_threshold();
		sleep(1);
    }
    printf("stopping pebs monitor for thread %d", cur_tid);

    return NULL;
}

void pebs_init(pid_t pid)
{
    // TODO add code that writes to /proc/sys/kernel/perf_event_paranoid ?

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
