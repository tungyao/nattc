#include "congestion.h"

uint32_t cbrt_fp(uint32_t x)
{
  if (x == 0) return 0;
  uint32_t y = x >> 5;
  if (y == 0) y = 1;
  for (int i = 0; i < 4; i++) {
    if (y == 0) { y = 1; break; }
    y = (uint32_t)((2 * (uint64_t)y + x / ((uint64_t)y * y)) / 3);
  }
  return y;
}

void cubic_init(struct cubic_state *cubic, struct delay_monitor *delay, uint32_t initial_rtt_ms)
{
  cubic->cwnd = CWND_INITIAL;
  cubic->ssthresh = SSTHRESH_INITIAL;
  cubic->w_max = 0;
  cubic->k = 0;
  cubic->last_loss_time = 0;
  cubic->cubic_c = CUBIC_C_Q16;
  cubic->in_recovery = 0;

  delay->base_rtt = initial_rtt_ms;
  delay->current_rtt = 0;
  delay->rtt_threshold = (uint32_t)((uint32_t)(initial_rtt_ms * 3 / 2) << 16);
  delay->delay_reduced = 0;
  delay->last_delay_reduce = 0;
}

void cubic_on_loss(struct cubic_state *cubic, uint32_t now_ms)
{
  cubic->w_max = cubic->cwnd;
  cubic->ssthresh = (uint32_t)((uint64_t)cubic->cwnd * 45875 >> 16);
  if (cubic->ssthresh < CWND_MIN)
    cubic->ssthresh = CWND_MIN;
  cubic->cwnd = cubic->ssthresh;
  cubic->in_recovery = 1;
  cubic->last_loss_time = now_ms;

  uint32_t w_max_scaled = (uint32_t)(((uint64_t)cubic->w_max * (65536 - 26214)) / 26214);
  cubic->k = cbrt_fp(w_max_scaled);
}
