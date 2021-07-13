
#include <memkind/internal/pebs.h>

#define SAMPLE_FREQUENCY 1000000
#define MMAP_DATA_SIZE   8 // TODO what is this?
#define rmb() asm volatile("lfence":::"memory")

pthread_t pebs_thread;
int pebs_fd;
static char *pebs_mmap;

/*
// defined in perfmon/perf_event.h
int perf_event_open(struct perf_event_attr *hw_event_uptr, pid_t pid, int cpu,
                    int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event_uptr, pid, cpu, group_fd,
                   flags);
}
*/

void *pebs_monitor(void *a)
{
    // set low priority
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    param.sched_priority = sched_get_priority_max(policy);
    pthread_setschedparam(pthread_self(), policy, &param);

    ioctl(pebs_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(pebs_fd, PERF_EVENT_IOC_ENABLE, 0);

    __u64 last_head = 0;

    while (1) {
        struct perf_event_mmap_page* pebs_metadata =
            (struct perf_event_mmap_page*)pebs_mmap;

        if (last_head < pebs_metadata->data_head) {
	        char *data_mmap = pebs_mmap + getpagesize();
            printf("new data!\n");

            while (last_head != pebs_metadata->data_head) {
                struct perf_event_header *event =
                    (struct perf_event_header *)data_mmap;

                switch (event->type) {
                    case PERF_RECORD_SAMPLE:
                    {
                         char* x = data_mmap + sizeof(struct perf_event_header);
                         printf("a: %p\n", (__u64*)x);
                         printf("head: %llu\n", pebs_metadata->data_head);
                    }
                    break;
                default:
                    break;
                };

                last_head += event->size;
                data_mmap += event->size;
            }
        }

        sleep(1);
        ioctl(pebs_fd, PERF_EVENT_IOC_REFRESH, 1);
        rmb();
    }

    return NULL;
}

void pebs_init()
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
    ret = pfm_get_os_event_encoding("MEM_UOPS_RETIRED:ALL_LOADS", PFM_PLM3, 
        PFM_OS_PERF_EVENT_EXT, &arg);
    if (ret != PFM_SUCCESS) {
        printf("pfm_get_os_event_encoding() failed!\n");
        exit(-1);
    }

    pe.size = sizeof(struct perf_event_attr);
    pe.sample_period = SAMPLE_FREQUENCY;
    pe.sample_type = PERF_SAMPLE_ADDR;

    pe.read_format = 0;
    pe.disabled = 1;
    pe.pinned = 1; // TODO comment what is this
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.wakeup_events = 1;

    pid_t pid = 0;          // measure current process
    int cpu = -1;           // .. on any CPU
    int group_fd = -1;      // use single event group
    pebs_fd = perf_event_open(&pe, pid, cpu, group_fd, 0);

    int mmap_pages = 1 + MMAP_DATA_SIZE;
    int map_size = mmap_pages * getpagesize();
    pebs_mmap = mmap(NULL, map_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, pebs_fd, 0);

    // printf("PEBS start\n");

    pthread_create(&pebs_thread, NULL, &pebs_monitor, NULL);
}

void pebs_fini()
{
    ioctl(pebs_fd, PERF_EVENT_IOC_DISABLE, 0);

    int mmap_pages = 1 + MMAP_DATA_SIZE;
    munmap(pebs_mmap, mmap_pages * getpagesize());
    close(pebs_fd);

    // printf("PEBS end\n");
}
