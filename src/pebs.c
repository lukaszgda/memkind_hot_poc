
#include <memkind/internal/pebs.h>
#include <memkind/internal/tachanka.h>
#include <memkind/internal/memkind_private.h>

#define SAMPLE_FREQUENCY 100000 // smaller value -> more frequent sampling
#define MMAP_DATA_SIZE   8
#define rmb() asm volatile("lfence":::"memory")

pthread_t pebs_thread;
int pebs_fd;
static char *pebs_mmap;

#define LOG_TO_FILE 1

void *pebs_monitor(void *a)
{
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
    if (pid != cur_pid) {
        char name[255] = {0};
        sprintf(name, "tier_pid_%d_tid_%d.log", cur_pid, cur_tid);
        log_file = open(name, O_CREAT | O_WRONLY, S_IRWXU);
    }
    lseek(log_file, 0, SEEK_END);
#endif

    while (1) {
        struct perf_event_mmap_page* pebs_metadata =
            (struct perf_event_mmap_page*)pebs_mmap;

        // must call this before read from data head
		rmb();

        // DEBUG
        // printf("head: %llu size: %lld\n", pebs_metadata->data_head, pebs_metadata->data_head - last_head);
        if (last_head < pebs_metadata->data_head) {
            printf("new data from PEBS!\n");

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
                        void* addr = (void*)(data_mmap + sizeof(struct perf_event_header) + sizeof(__u64));
                        // DEBUG
                        //sprintf(buf, "last: %llu, head: %llu t: %llu x: %llx\n",
                        //    last_head, pebs_metadata->data_head,
                        //    timestamp, addr);
                        //printf("%s", buf);
                        touch(addr, timestamp);
#if LOG_TO_FILE
                        if (write(log_file, buf, strlen(buf))) ;
#endif
                    }
                    break;
                default:
                    break;
                };

                last_head += event->size;
                data_mmap += event->size;
            }
        }

		ioctl(pebs_fd, PERF_EVENT_IOC_REFRESH, 0);
        last_head = pebs_metadata->data_head;
        pebs_metadata->data_tail = pebs_metadata->data_head;

		sleep(1);
    }

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
   // char* event[] = "MEM_UOPS_RETIRED:ALL_LOADS";

    ret = pfm_get_os_event_encoding(event, PFM_PLM3, PFM_OS_PERF_EVENT_EXT, &arg);
    if (ret != PFM_SUCCESS) {
        printf("pfm_get_os_event_encoding() failed!\n");
        exit(-1);
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

    int mmap_pages = 1 + MMAP_DATA_SIZE;
    int map_size = mmap_pages * getpagesize();
    pebs_mmap = mmap(NULL, map_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, pebs_fd, 0);

    // DEBUG
    //printf("PEBS thread start\n");

    pthread_create(&pebs_thread, NULL, &pebs_monitor, NULL);

	ioctl(pebs_fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(pebs_fd, PERF_EVENT_IOC_ENABLE, 0);
}

void pebs_fini()
{
    ioctl(pebs_fd, PERF_EVENT_IOC_DISABLE, 0);

    int mmap_pages = 1 + MMAP_DATA_SIZE;
    munmap(pebs_mmap, mmap_pages * getpagesize());
    close(pebs_fd);

    // DEBUG
    //printf("PEBS thread end\n");
}

MEMKIND_EXPORT void pebs_fork(pid_t pid)
{
    pebs_init(pid);
}
