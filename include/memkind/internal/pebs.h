
#include <pthread.h>

// TODO remove unused headers
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <asm/unistd.h>

#include <linux/hw_breakpoint.h> /* Definition of HW_* constants */
#include <linux/perf_event.h>    /* Definition of PERF_* constants */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>         /* Definition of SYS_* constants */

#include <perfmon/pfmlib.h>     // pfm_get_os_event_encoding
#include <perfmon/pfmlib_perf_event.h>


void *pebs_monitor(void *);

void pebs_init();
void pebs_fini();
