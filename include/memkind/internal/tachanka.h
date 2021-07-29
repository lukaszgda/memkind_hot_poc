int is_hot(uint64_t hash);
void register_block(uint64_t hash, void *addr, size_t size);
void *new_block(size_t size);
void touch(void *addr);
void tachanka_init(void);
