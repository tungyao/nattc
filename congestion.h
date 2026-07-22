#ifndef CONGESTION_H
#define CONGESTION_H

#include <stdint.h>

#define CUBIC_C_Q16 26214
#define CUBIC_MSS 1200
#define CWND_INITIAL 12000
#define CWND_MIN 2400
#define SSTHRESH_INITIAL 65536

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
};

uint32_t cbrt_fp(uint32_t x);
void cubic_init(struct cubic_state *cubic, struct delay_monitor *delay, uint32_t initial_rtt_ms);
void cubic_on_loss(struct cubic_state *cubic, uint32_t now_ms);

#endif
