#include "token_bucket.h"
#include "utils.h"
#include <string.h>

void token_bucket_init(struct token_bucket *tb, uint64_t fill_rate) {
    memset(tb, 0, sizeof(*tb));
    tb->fill_rate = fill_rate;
    tb->capacity = fill_rate * 200 / 1000; /* 200ms worth */
    if (tb->capacity == 0) tb->capacity = 1;
    tb->tokens = tb->capacity;
    tb->last_refill_ms = get_time_ms();
}

void token_bucket_refill(struct token_bucket *tb) {
    int64_t now_ms = get_time_ms();
    int64_t elapsed = now_ms - tb->last_refill_ms;
    if (elapsed <= 0) return;

    uint64_t to_add = (tb->fill_rate * (uint64_t)elapsed) / 1000;
    if (to_add > 0) {
        tb->tokens += to_add;
        if (tb->tokens > tb->capacity)
            tb->tokens = tb->capacity;
    }
    tb->last_refill_ms = now_ms;
}

bool token_bucket_try_consume(struct token_bucket *tb, uint64_t tokens) {
    if (tb->tokens < tokens) return false;
    tb->tokens -= tokens;
    return true;
}

int64_t token_bucket_wait_ms(struct token_bucket *tb, uint64_t tokens) {
    if (tb->tokens >= tokens) return 0;
    uint64_t deficit = tokens - tb->tokens;
    int64_t ms = (int64_t)((deficit * 1000 + tb->fill_rate - 1) / tb->fill_rate);
    return ms > 0 ? ms : 1;
}
