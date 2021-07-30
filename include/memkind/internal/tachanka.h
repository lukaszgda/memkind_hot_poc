int is_hot(uint64_t hash);
void register_block(uint64_t hash, void *addr, size_t size);
void *new_block(size_t size);
void touch(void *addr, __u64 timestamp);
void tachanka_init(void);
