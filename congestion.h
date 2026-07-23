#ifndef CONGESTION_H
#define CONGESTION_H

#include <stdint.h>

/* MSS for congestion calculations */
#define CUBIC_MSS 1200

/* Congestion window bounds (in bytes) */
#define CWND_INITIAL (40 * CUBIC_MSS)   /* 48000 */
#define CWND_MIN     (10 * CUBIC_MSS)   /* 12000 */
#define SSTHRESH_INITIAL (220 * CUBIC_MSS) /* 264000 */

/* RTT constants */
#define INITIAL_RTT_MS 10
#define MIN_RTO_MS 50
#define MAX_RTO_MS 2000

/* Reno congestion control states */
enum reno_state {
  RENO_SLOW_START,
  RENO_CONGESTION_AVOIDANCE,
  RENO_FAST_RECOVERY
};

/* Reno AIMD congestion state */
struct congestion_state {
  enum reno_state state;
  uint32_t cwnd;           /* congestion window in bytes */
  uint32_t ssthresh;       /* slow start threshold in bytes */
  uint32_t last_loss_time; /* timestamp of last loss event */
};

void reno_init(struct congestion_state *cs);
void reno_on_loss(struct congestion_state *cs, uint32_t now_ms, int is_timeout);
void reno_on_ack(struct congestion_state *cs, uint32_t bytes_acked, uint32_t largest_acked, uint32_t recovery_end_seq);
uint32_t reno_get_cwnd(struct congestion_state *cs);

#endif /* CONGESTION_H */
