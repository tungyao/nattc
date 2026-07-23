#include "fec.h"
#include <string.h>
#include <stdlib.h>

#define GF_PRIMITIVE 0x11D

static uint8_t gf_log[256];
static uint8_t gf_exp[512];
static int gf_tables_initialized = 0;

static void gf_init_tables(void)
{
  if (gf_tables_initialized) return;
  gf_tables_initialized = 1;
  uint16_t x = 1;
  for (int i = 0; i < 255; i++) {
    gf_exp[i] = (uint8_t)x;
    gf_log[x] = (uint8_t)i;
    x <<= 1;
    if (x & 0x100) x ^= GF_PRIMITIVE;
    x &= 0xFF;
  }
  for (int i = 255; i < 512; i++)
    gf_exp[i] = gf_exp[i - 255];
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
  if (a == 0 || b == 0) return 0;
  uint16_t sum = (uint16_t)gf_log[a] + (uint16_t)gf_log[b];
  if (sum >= 255) sum -= 255;
  return gf_exp[sum];
}

static uint8_t gf_inv(uint8_t a)
{
  if (a == 0) return 0;
  return gf_exp[255 - gf_log[a]];
}

static uint8_t vandermonde(uint8_t i, uint8_t j)
{
  return gf_exp[(i * j) % 255];
}

static int gf_matrix_invert(uint8_t *result, const uint8_t *matrix, int n)
{
  uint8_t aug[FEC_MAX_N * FEC_MAX_N * 2];
  int size = n * 2;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++)
      aug[i * size + j] = matrix[i * n + j];
    for (int j = n; j < size; j++)
      aug[i * size + j] = (j - n == i) ? 1 : 0;
  }
  for (int col = 0; col < n; col++) {
    int pivot = -1;
    for (int row = col; row < n; row++) {
      if (aug[row * size + col] != 0) { pivot = row; break; }
    }
    if (pivot < 0) return -1;
    if (pivot != col) {
      for (int j = 0; j < size; j++) {
        uint8_t tmp = aug[col * size + j];
        aug[col * size + j] = aug[pivot * size + j];
        aug[pivot * size + j] = tmp;
      }
    }
    uint8_t inv_pivot = gf_inv(aug[col * size + col]);
    for (int j = 0; j < size; j++)
      aug[col * size + j] = gf_mul(aug[col * size + j], inv_pivot);
    for (int row = 0; row < n; row++) {
      if (row == col) continue;
      uint8_t factor = aug[row * size + col];
      if (factor == 0) continue;
      for (int j = 0; j < size; j++)
        aug[row * size + j] ^= gf_mul(factor, aug[col * size + j]);
    }
  }
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      result[i * n + j] = aug[i * size + n + j];
  return 0;
}

void fec_init(struct fec_ctx *ctx, uint8_t n, uint8_t m)
{
  gf_init_tables();
  ctx->n = n;
  ctx->m = m;
  ctx->gf_log = gf_log;
  ctx->gf_exp = gf_exp;
  ctx->gf_pow = NULL;
}

void fec_free(struct fec_ctx *ctx)
{
  (void)ctx;
}

int fec_set_params(struct fec_ctx *ctx, uint8_t n, uint8_t m)
{
  if (n > FEC_MAX_N || m > FEC_MAX_M || n == 0 || m == 0)
    return -1;
  ctx->n = n;
  ctx->m = m;
  return 0;
}

uint16_t fec_padded_length(struct fec_packet *packets, uint8_t count)
{
  uint16_t max_len = 0;
  for (uint8_t i = 0; i < count; i++)
    if (packets[i].len > max_len) max_len = packets[i].len;
  if (max_len > FEC_MAX_PACKET_SIZE) max_len = FEC_MAX_PACKET_SIZE;
  return max_len;
}

int fec_encode(struct fec_ctx *ctx, struct fec_packet *data_packets,
               struct fec_packet *parity_packets, uint16_t padded_len)
{
  uint8_t n = ctx->n;
  uint8_t m = ctx->m;
  if (padded_len > FEC_MAX_PACKET_SIZE || n == 0 || m == 0) return -1;
  for (uint8_t i = 0; i < m; i++) {
    memset(parity_packets[i].data, 0, padded_len);
    parity_packets[i].len = padded_len;
    for (uint16_t b = 0; b < padded_len; b++) {
      uint8_t sum = 0;
      for (uint8_t j = 0; j < n; j++) {
        uint8_t d = (b < data_packets[j].len) ? data_packets[j].data[b] : 0;
        sum ^= gf_mul(vandermonde(i, j), d);
      }
      parity_packets[i].data[b] = sum;
    }
  }
  return 0;
}

int fec_decode(struct fec_ctx *ctx, struct fec_packet *packets,
               uint8_t *missing_indices, uint8_t missing_count,
               uint16_t padded_len)
{
  uint8_t n = ctx->n;
  if (missing_count > ctx->m) return -1;
  if (padded_len > FEC_MAX_PACKET_SIZE || n == 0) return -1;
  uint8_t total = n + ctx->m;
  uint8_t row_indices[FEC_MAX_N];
  uint8_t row_count = 0;
  for (uint8_t i = 0; i < total && row_count < n; i++) {
    int is_missing = 0;
    for (uint8_t k = 0; k < missing_count; k++) {
      if (missing_indices[k] == i) { is_missing = 1; break; }
    }
    if (!is_missing)
      row_indices[row_count++] = i;
  }
  if (row_count < n) return -1;
  uint8_t A[FEC_MAX_N * FEC_MAX_N];
  for (uint8_t r = 0; r < n; r++) {
    uint8_t ri = row_indices[r];
    for (uint8_t c = 0; c < n; c++) {
      if (ri < n)
        A[r * n + c] = (ri == c) ? 1 : 0;
      else
        A[r * n + c] = vandermonde(ri - n, c);
    }
  }
  uint8_t inv[FEC_MAX_N * FEC_MAX_N];
  if (gf_matrix_invert(inv, A, n) != 0) return -1;
  uint8_t tmp[FEC_MAX_N];
  for (uint16_t b = 0; b < padded_len; b++) {
    for (uint8_t r = 0; r < n; r++) {
      uint8_t ri = row_indices[r];
      tmp[r] = (b < packets[ri].len) ? packets[ri].data[b] : 0;
    }
    for (uint8_t r = 0; r < n; r++) {
      uint8_t sum = 0;
      for (uint8_t c = 0; c < n; c++)
        sum ^= gf_mul(inv[r * n + c], tmp[c]);
      if (b < FEC_MAX_PACKET_SIZE)
        packets[r].data[b] = sum;
    }
  }
  for (uint8_t k = 0; k < missing_count; k++) {
    uint8_t idx = missing_indices[k];
    if (idx < n)
      packets[idx].len = padded_len;
  }
  return 0;
}

uint8_t fec_adaptive_m(float loss_rate)
{
  if (loss_rate < 0.01f) return 1;
  if (loss_rate < 0.03f) return 2;
  if (loss_rate < 0.06f) return 3;
  if (loss_rate < 0.10f) return 4;
  return 5;
}

uint8_t fec_adaptive_n(uint32_t smoothed_rtt_ms)
{
  if (smoothed_rtt_ms < 30)  return 20;
  if (smoothed_rtt_ms < 80)  return 15;
  if (smoothed_rtt_ms < 150) return 10;
  if (smoothed_rtt_ms < 300) return 8;
  return 6;
}
