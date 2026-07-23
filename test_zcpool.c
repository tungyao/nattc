#include "zcpool.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    struct zcpool pool;

    /* Test 1: init */
    zcpool_init(&pool);
    assert(pool.free_count == ZCPOOL_POOL_SIZE);

    /* Test 2: alloc returns buffer */
    struct zcpool_buf *b1 = zcpool_alloc(&pool);
    assert(b1 != NULL);
    assert(pool.free_count == ZCPOOL_POOL_SIZE - 1);
    assert(b1->offset == 0);
    assert(b1->len == 0);
    assert(b1->refcount == 1);

    /* Test 3: payload points to data+offset */
    void *p = zcpool_payload(b1);
    assert(p == b1->data);

    /* Test 4: write at offset */
    b1->offset = 12;
    p = zcpool_payload(b1);
    assert(p == b1->data + 12);

    /* Test 5: free returns to pool */
    zcpool_free(&pool, b1);
    assert(pool.free_count == ZCPOOL_POOL_SIZE);

    /* Test 6: exhaust pool */
    struct zcpool_buf *bufs[ZCPOOL_POOL_SIZE];
    for (int i = 0; i < ZCPOOL_POOL_SIZE; i++) {
        bufs[i] = zcpool_alloc(&pool);
        assert(bufs[i] != NULL);
    }
    assert(pool.free_count == 0);
    assert(zcpool_alloc(&pool) == NULL);

    /* Test 7: free all */
    for (int i = 0; i < ZCPOOL_POOL_SIZE; i++) {
        zcpool_free(&pool, bufs[i]);
    }
    assert(pool.free_count == ZCPOOL_POOL_SIZE);

    /* Test 8: refcount */
    b1 = zcpool_alloc(&pool);
    b1->refcount = 3;
    zcpool_free(&pool, b1);
    assert(pool.free_count == ZCPOOL_POOL_SIZE - 1); /* not freed yet */
    zcpool_free(&pool, b1);
    assert(pool.free_count == ZCPOOL_POOL_SIZE - 1); /* still not freed */
    zcpool_free(&pool, b1);
    assert(pool.free_count == ZCPOOL_POOL_SIZE); /* now freed */

    printf("All zcpool tests passed!\n");
    return 0;
}
