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

static double cbrt_double(double x)
{
  if (x == 0.0) return 0.0;
  double y = x / 3.0;
  for (int i = 0; i < 20; i++) {
    double y2 = y * y;
    double y_next = (2.0 * y + x / y2) / 3.0;
    double diff = y_next - y;
    if (diff < 0) diff = -diff;
    if (diff < 1e-12 * y) break;
    y = y_next;
  }
  return y;
}

void cubic_init(struct cubic_state *cubic, struct delay_monitor *delay, uint32_t initial_rtt_ms)
{
  cubic->cwnd = CWND_INITIAL;
  cubic->ssthresh = SSTHRESH_INITIAL;
  cubic->w_max = 0;
  cubic->k_q16 = 0;
  cubic->last_loss_time = 0;
  cubic->cubic_c = CUBIC_C_Q16;
  cubic->in_recovery = 0;

  delay->base_rtt = initial_rtt_ms;
  delay->current_rtt = 0;
  delay->rtt_threshold = (uint32_t)((uint32_t)(initial_rtt_ms * 3 / 2) << 16);
  delay->delay_reduced = 0;
  delay->last_delay_reduce = 0;
  delay->high_rtt_count = 0;
  delay->delay_recovery_count = 0;
}

static void cubic_recalc_k(struct cubic_state *cubic)
{
  double w_max_seg = (double)cubic->w_max / (double)CUBIC_MSS;
  double k_sec = cbrt_double(w_max_seg * 0.75);
  cubic->k_q16 = (uint32_t)(k_sec * 65536.0 + 0.5);
}

void cubic_on_loss(struct cubic_state *cubic, uint32_t now_ms, int is_congestion_loss)
{
  uint32_t reduce_num = is_congestion_loss ? CUBIC_LOSS_REDUCE_NUM : CUBIC_RANDOM_LOSS_REDUCE_NUM;
  uint32_t reduce_den = is_congestion_loss ? CUBIC_LOSS_REDUCE_DEN : CUBIC_RANDOM_LOSS_REDUCE_DEN;

  cubic->w_max = cubic->cwnd;
  cubic->ssthresh = (uint32_t)((uint64_t)cubic->cwnd * reduce_num / reduce_den);
  if (cubic->ssthresh < CWND_MIN)
    cubic->ssthresh = CWND_MIN;
  cubic->cwnd = cubic->ssthresh;
  cubic->in_recovery = 1;
  cubic->last_loss_time = now_ms;

  cubic_recalc_k(cubic);
}

void cubic_on_delay_reduction(struct cubic_state *cubic, uint32_t now_ms)
{
  cubic->w_max = cubic->cwnd;
  cubic->cwnd = (uint32_t)((uint64_t)cubic->cwnd * DELAY_REDUCE_FACTOR_NUM / DELAY_REDUCE_FACTOR_DEN);
  if (cubic->cwnd < CWND_MIN)
    cubic->cwnd = CWND_MIN;
  cubic->ssthresh = cubic->cwnd;
  cubic->last_loss_time = now_ms;

  cubic_recalc_k(cubic);
}
