#include "zcpool.h"
#include <string.h>

void zcpool_init(struct zcpool *pool) {
    pool->free_count = ZCPOOL_POOL_SIZE;
    for (int i = 0; i < ZCPOOL_POOL_SIZE; i++) {
        pool->free_list[i] = i;
        memset(&pool->pool[i], 0, sizeof(struct zcpool_buf));
    }
}

struct zcpool_buf* zcpool_alloc(struct zcpool *pool) {
    if (pool->free_count <= 0) return NULL;
    int idx = pool->free_list[--pool->free_count];
    struct zcpool_buf *buf = &pool->pool[idx];
    buf->offset = 0;
    buf->len = 0;
    buf->refcount = 1;
    return buf;
}

void zcpool_free(struct zcpool *pool, struct zcpool_buf *buf) {
    if (!buf) return;
    buf->refcount--;
    if (buf->refcount > 0) return;
    int idx = (int)(buf - pool->pool);
    if (idx < 0 || idx >= ZCPOOL_POOL_SIZE) return;
    pool->free_list[pool->free_count++] = idx;
    memset(buf->data, 0, ZCPOOL_BUF_SIZE);
    buf->offset = 0;
    buf->len = 0;
}

void* zcpool_payload(struct zcpool_buf *buf) {
    return buf->data + buf->offset;
}

void zcpool_reset(struct zcpool_buf *buf) {
    buf->offset = 0;
    buf->len = 0;
    buf->refcount = 1;
}
