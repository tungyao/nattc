#ifndef ZCPOOL_H
#define ZCPOOL_H

#include <stdint.h>
#include <stddef.h>

#define ZCPOOL_BUF_SIZE  2048
#define ZCPOOL_POOL_SIZE 256

struct zcpool_buf {
    char     data[ZCPOOL_BUF_SIZE];
    uint16_t offset;
    uint16_t len;
    int      refcount;
};

struct zcpool {
    struct zcpool_buf pool[ZCPOOL_POOL_SIZE];
    int free_list[ZCPOOL_POOL_SIZE];
    int free_count;
};

void              zcpool_init(struct zcpool *pool);
struct zcpool_buf* zcpool_alloc(struct zcpool *pool);
void              zcpool_free(struct zcpool *pool, struct zcpool_buf *buf);
void*             zcpool_payload(struct zcpool_buf *buf);
void              zcpool_reset(struct zcpool_buf *buf);

#endif /* ZCPOOL_H */
