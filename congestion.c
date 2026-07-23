#include "congestion.h"

void reno_init(struct congestion_state *cs)
{
  cs->state = RENO_SLOW_START;
  cs->cwnd = CWND_INITIAL;
  cs->ssthresh = SSTHRESH_INITIAL;
  cs->last_loss_time = 0;
}

void reno_on_loss(struct congestion_state *cs, uint32_t now_ms, int is_timeout)
{
  cs->ssthresh = cs->cwnd / 2;
  if (cs->ssthresh < CWND_MIN)
    cs->ssthresh = CWND_MIN;

  if (is_timeout) {
    cs->cwnd = CWND_INITIAL;
    cs->state = RENO_SLOW_START;
  } else {
    cs->cwnd = cs->ssthresh;
    cs->state = RENO_FAST_RECOVERY;
  }

  cs->last_loss_time = now_ms;
}

void reno_on_ack(struct congestion_state *cs, uint32_t bytes_acked,
                 uint32_t largest_acked, uint32_t recovery_end_seq)
{
  if (cs->state == RENO_FAST_RECOVERY) {
    if (largest_acked >= recovery_end_seq) {
      cs->cwnd = cs->ssthresh;
      cs->state = RENO_CONGESTION_AVOIDANCE;
    }
    return;
  }

  if (cs->state == RENO_SLOW_START) {
    cs->cwnd += bytes_acked;
    if (cs->cwnd >= cs->ssthresh)
      cs->state = RENO_CONGESTION_AVOIDANCE;
  } else {
    uint32_t increase = (uint32_t)((uint64_t)CUBIC_MSS * bytes_acked / cs->cwnd);
    if (increase == 0) increase = 1;
    cs->cwnd += increase;
  }
}

uint32_t reno_get_cwnd(struct congestion_state *cs)
{
  return cs->cwnd;
}
