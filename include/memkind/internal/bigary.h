#define BIGARY_DRAM -1, MAP_ANONYMOUS|MAP_PRIVATE

struct bigary
{
    void *area;
    size_t declared;
    size_t top;
    pthread_mutex_t enlargement;
};
typedef struct bigary bigary;

void bigary_init(bigary *restrict ba, int fd, int flags, size_t max);
void bigary_alloc(bigary *restrict ba, size_t top);
void bigary_free(bigary *restrict ba);
