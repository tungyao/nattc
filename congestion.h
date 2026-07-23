#ifndef CONGESTION_H
#define CONGESTION_H

#include <stdint.h>

#define CUBIC_C_Q16 26214
#define CUBIC_MSS 1200
#define CWND_INITIAL 48000
#define CWND_MIN 12000
#define SSTHRESH_INITIAL 262144

#define DELAY_REDUCE_FACTOR_NUM 58982
#define DELAY_REDUCE_FACTOR_DEN 65536
#define DELAY_CONSECUTIVE_THRESHOLD 3
#define DELAY_REDUCE_MULTIPLIER_RTT 2

#define CUBIC_LOSS_REDUCE_NUM 45875
#define CUBIC_LOSS_REDUCE_DEN 65536
#define CUBIC_RANDOM_LOSS_REDUCE_NUM 58982
#define CUBIC_RANDOM_LOSS_REDUCE_DEN 65536

struct cubic_state {
  uint32_t cwnd;
  uint32_t ssthresh;
  uint32_t w_max;
  uint32_t k_q16;
  uint32_t last_loss_time;
  uint32_t cubic_c;
  uint8_t  in_recovery;
};

struct delay_monitor {
  uint32_t base_rtt;
  uint32_t current_rtt;
  uint32_t rtt_threshold;
  uint8_t  delay_reduced;
  uint32_t last_delay_reduce;
  uint8_t  high_rtt_count;
  uint8_t  delay_recovery_count;
};

uint32_t cbrt_fp(uint32_t x);
void cubic_init(struct cubic_state *cubic, struct delay_monitor *delay, uint32_t initial_rtt_ms);
void cubic_on_loss(struct cubic_state *cubic, uint32_t now_ms, int is_congestion_loss);
void cubic_on_delay_reduction(struct cubic_state *cubic, uint32_t now_ms);

#endif
