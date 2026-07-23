#ifndef TOKEN_BUCKET_H
#define TOKEN_BUCKET_H

#include <stdint.h>
#include <stdbool.h>

struct token_bucket {
    uint64_t tokens;
    uint64_t capacity;
    uint64_t fill_rate;     /* bytes per second */
    int64_t  last_refill_ms;
};

void    token_bucket_init(struct token_bucket *tb, uint64_t fill_rate);
void    token_bucket_refill(struct token_bucket *tb);
bool    token_bucket_try_consume(struct token_bucket *tb, uint64_t tokens);
int64_t token_bucket_wait_ms(struct token_bucket *tb, uint64_t tokens);

#endif /* TOKEN_BUCKET_H */
