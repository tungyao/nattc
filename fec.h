#ifndef FEC_H
#define FEC_H

#include <stdint.h>
#include <stddef.h>

#define FEC_DEFAULT_N 10
#define FEC_DEFAULT_M 2
#define FEC_MAX_PACKET_SIZE 1200
#define FEC_MAX_N 20
#define FEC_MAX_M 5
#define FEC_MAX_GROUP_SIZE (FEC_MAX_N + FEC_MAX_M)
#define FEC_ADAPT_INTERVAL 100

struct fec_packet {
  uint8_t data[FEC_MAX_PACKET_SIZE];
  uint16_t len;
};

struct fec_ctx {
  uint8_t n;
  uint8_t m;
  uint8_t *gf_log;
  uint8_t *gf_exp;
};

struct fec_send_group {
  uint8_t count;
  uint32_t group_id;
  uint32_t packet_seqs[FEC_MAX_N];
  uint16_t stream_ids[FEC_MAX_N];
  uint32_t stream_seqs[FEC_MAX_N];
  uint16_t data_lengths[FEC_MAX_N];
  struct fec_packet packets[FEC_MAX_N];
};

struct fec_recv_group {
  uint8_t active;
  uint32_t group_id;
  uint8_t received_count;
  uint64_t received_bitmask;
  uint32_t packet_seqs[FEC_MAX_GROUP_SIZE];
  uint16_t stream_ids[FEC_MAX_GROUP_SIZE];
  uint32_t stream_seqs[FEC_MAX_GROUP_SIZE];
  uint16_t data_lengths[FEC_MAX_GROUP_SIZE];
  struct fec_packet packets[FEC_MAX_GROUP_SIZE];
};

void fec_init(struct fec_ctx *ctx, uint8_t n, uint8_t m);
void fec_free(struct fec_ctx *ctx);
int fec_set_params(struct fec_ctx *ctx, uint8_t n, uint8_t m);

uint16_t fec_padded_length(struct fec_packet *packets, uint8_t count);

int fec_encode(struct fec_ctx *ctx, struct fec_packet *data_packets,
               struct fec_packet *parity_packets, uint16_t padded_len);

int fec_decode(struct fec_ctx *ctx, struct fec_packet *packets,
               uint8_t *missing_indices, uint8_t missing_count,
               uint16_t padded_len);

uint8_t fec_adaptive_m(float loss_rate);

#endif
