#include "token_bucket.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    struct token_bucket tb;

    /* Test 1: init fills to capacity */
    token_bucket_init(&tb, 1000000); /* 1 MB/s */
    assert(tb.tokens == tb.capacity);
    assert(tb.capacity == 200000); /* 1MB/s * 200ms / 1000 = 200000 bytes */

    /* Test 2: consume succeeds when tokens available */
    assert(token_bucket_try_consume(&tb, 100) == true);
    assert(tb.tokens == 199900); /* 200000 - 100 */

    /* Test 3: consume fails when tokens insufficient */
    assert(token_bucket_try_consume(&tb, 200000) == false);
    assert(tb.tokens == 199900); /* unchanged */

    /* Test 4: wait_ms returns 0 when tokens available */
    assert(token_bucket_wait_ms(&tb, 100) == 0);

    /* Test 5: wait_ms returns positive when tokens insufficient */
    int64_t wait = token_bucket_wait_ms(&tb, 500000);
    assert(wait > 0);

    /* Test 6: refill adds tokens */
    tb.last_refill_ms -= 100; /* simulate 100ms elapsed */
    token_bucket_refill(&tb);
    assert(tb.tokens > 199900); /* should have added ~100000 bytes */

    /* Test 7: refill caps at capacity */
    tb.tokens = tb.capacity;
    tb.last_refill_ms -= 1000;
    token_bucket_refill(&tb);
    assert(tb.tokens == tb.capacity);

    printf("All token_bucket tests passed!\n");
    return 0;
}
