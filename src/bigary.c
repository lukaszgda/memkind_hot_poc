#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "memkind/internal/bigary.h"

#define BIGARY_DEFAULT_MAX (16 * 1024 * 1048576ULL)
#define BIGARY_PAGESIZE 2097152

static void die(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (write(2, buf, len));
    exit(1);
}

/***************************/
/* initialize a new bigary */
/***************************/
void bigary_init(bigary *restrict ba, int fd, int flags, size_t max)
{
    if (!max)
        max = BIGARY_DEFAULT_MAX;
    int ret = pthread_mutex_init(&ba->enlargement, 0);
    if (ret != 0)
        die("mutex init failed\n");
    ba->declared = max;
    ba->fd = fd;
    ba->flags = flags;
    if ((ba->area = mmap(0, max, PROT_NONE, flags, fd, 0)) == MAP_FAILED)
        die("mmapping bigary(%zd) failed: %m\n", max);
    if (mmap(ba->area, BIGARY_PAGESIZE, PROT_READ|PROT_WRITE,
        MAP_FIXED|flags, fd, 0) == MAP_FAILED)
    {
        die("bigary alloc of %zd failed: %m\n", BIGARY_PAGESIZE);
    }
    ba->top = BIGARY_PAGESIZE;
}

/********************************************************************/
/* ensure there's at least X space allocated                        */
/* (you may want MAP_POPULATE to ensure the space is actually there */
/********************************************************************/
void bigary_alloc(bigary *restrict ba, size_t top)
{
    if (ba->top >= top)
        return;
    pthread_mutex_lock(&ba->enlargement);
    if (ba->top >= top) // re-check
        goto done;
    top = (top + BIGARY_PAGESIZE - 1) & ~(BIGARY_PAGESIZE - 1); // align up
    // printf("extending to %zd\n", top);
    if (top > ba->declared)
        die("bigary's max is %zd, %zd requested.\n", ba->declared, top);
    if (mmap(ba->area + ba->top, top - ba->top, PROT_READ|PROT_WRITE,
        MAP_FIXED|ba->flags, ba->fd, ba->top) == MAP_FAILED)
    {
        die("in-bigary alloc of %zd to %zd failed: %m\n", top - ba->top, top);
    }
    ba->top = top;
done:
    pthread_mutex_unlock(&ba->enlargement);
}

void bigary_free(bigary *restrict ba)
{
    pthread_mutex_destroy(&ba->enlargement);
    munmap(ba->area, ba->declared);
}
