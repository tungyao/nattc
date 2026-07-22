#include "frame.h"
#include <string.h>

/* Network byte order conversion helpers */
#ifndef htons
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#endif

int frame_serialize_header(uint8_t *buf, const struct frame_header *hdr)
{
  if (!buf || !hdr) return -1;
  buf[0] = hdr->version & 0xFF;
  buf[1] = ((hdr->priority & 1) << 7)
         | ((hdr->reserved & 1) << 6)
         | (hdr->type & 0x3F);
  buf[2] = (uint8_t)((hdr->stream_id >> 8) & 0xFF);
  buf[3] = (uint8_t)(hdr->stream_id & 0xFF);
  return FRAME_HEADER_SIZE;
}

int frame_deserialize_header(struct frame_header *hdr, const uint8_t *buf)
{
  if (!hdr || !buf) return -1;
  hdr->version = buf[0];
  hdr->priority = (buf[1] >> 7) & 1;
  hdr->reserved = (buf[1] >> 6) & 1;
  hdr->type = buf[1] & 0x3F;
  hdr->stream_id = ((uint16_t)buf[2] << 8) | buf[3];
  return FRAME_HEADER_SIZE;
}

/* DATA body: packet_seq(4) + stream_seq(4) + data_len(2) + payload */
int frame_serialize_data_body(uint8_t *buf, const struct frame_data *data)
{
  if (!buf || !data) return -1;
  uint32_t ps = data->packet_seq;
  uint32_t ss = data->stream_seq;
  uint16_t dl = data->data_len;
  buf[0] = (uint8_t)(ps >> 24);
  buf[1] = (uint8_t)(ps >> 16);
  buf[2] = (uint8_t)(ps >> 8);
  buf[3] = (uint8_t)(ps & 0xFF);
  buf[4] = (uint8_t)(ss >> 24);
  buf[5] = (uint8_t)(ss >> 16);
  buf[6] = (uint8_t)(ss >> 8);
  buf[7] = (uint8_t)(ss & 0xFF);
  buf[8] = (uint8_t)(dl >> 8);
  buf[9] = (uint8_t)(dl & 0xFF);
  return FRAME_DATA_HEADER_SIZE;
}

int frame_deserialize_data_body(struct frame_data *data, const uint8_t *buf)
{
  if (!data || !buf) return -1;
  data->packet_seq = ((uint32_t)buf[0] << 24)
                   | ((uint32_t)buf[1] << 16)
                   | ((uint32_t)buf[2] << 8)
                   | buf[3];
  data->stream_seq = ((uint32_t)buf[4] << 24)
                   | ((uint32_t)buf[5] << 16)
                   | ((uint32_t)buf[6] << 8)
                   | buf[7];
  data->data_len = ((uint16_t)buf[8] << 8) | buf[9];
  return FRAME_DATA_HEADER_SIZE;
}

/* ACK body: largest_acked(4) + ack_delay(2) + first_ack_range(2)
 *          + range_count(2) + ranges(range_count * 4 each) */
int frame_serialize_ack_body(uint8_t *buf, const struct frame_ack *ack,
                              const struct ack_range *ranges)
{
  if (!buf || !ack || (ack->range_count > 0 && !ranges)) return -1;
  uint32_t la = ack->largest_acked;
  buf[0] = (uint8_t)(la >> 24);
  buf[1] = (uint8_t)(la >> 16);
  buf[2] = (uint8_t)(la >> 8);
  buf[3] = (uint8_t)(la & 0xFF);
  uint16_t ad = ack->ack_delay;
  buf[4] = (uint8_t)(ad >> 8);
  buf[5] = (uint8_t)(ad & 0xFF);
  uint16_t fa = ack->first_ack_range;
  buf[6] = (uint8_t)(fa >> 8);
  buf[7] = (uint8_t)(fa & 0xFF);
  uint16_t rc = ack->range_count;
  buf[8] = (uint8_t)(rc >> 8);
  buf[9] = (uint8_t)(rc & 0xFF);
  int off = FRAME_ACK_FIXED_SIZE;
  for (uint16_t i = 0; i < rc; i++) {
    uint16_t g = ranges[i].gap;
    uint16_t l = ranges[i].length;
    buf[off + 0] = (uint8_t)(g >> 8);
    buf[off + 1] = (uint8_t)(g & 0xFF);
    buf[off + 2] = (uint8_t)(l >> 8);
    buf[off + 3] = (uint8_t)(l & 0xFF);
    off += 4;
  }
  return off;
}

int frame_deserialize_ack_body(struct frame_ack *ack, struct ack_range *ranges,
                                int max_ranges, const uint8_t *buf, uint16_t buf_len)
{
  if (!ack || !buf) return -1;
  if (buf_len < FRAME_ACK_FIXED_SIZE) return -1;
  ack->largest_acked = ((uint32_t)buf[0] << 24)
                     | ((uint32_t)buf[1] << 16)
                     | ((uint32_t)buf[2] << 8)
                     | buf[3];
  ack->ack_delay = ((uint16_t)buf[4] << 8) | buf[5];
  ack->first_ack_range = ((uint16_t)buf[6] << 8) | buf[7];
  ack->range_count = ((uint16_t)buf[8] << 8) | buf[9];
  if (ack->range_count > 0 && !ranges) return -1;
  int off = FRAME_ACK_FIXED_SIZE;
  int max_entries = (buf_len - FRAME_ACK_FIXED_SIZE) / 4;
  if (max_entries < ack->range_count) return -1;
  if (max_ranges > 0 && ack->range_count > (uint16_t)max_ranges) return -1;
  for (uint16_t i = 0; i < ack->range_count; i++) {
    ranges[i].gap = ((uint16_t)buf[off] << 8) | buf[off + 1];
    ranges[i].length = ((uint16_t)buf[off + 2] << 8) | buf[off + 3];
    off += 4;
  }
  return off;
}

/* STREAM_OPEN body: stream_id(2) + flags(1) [R|O|reserved(4)|priority(2)] + reserved(1) */
int frame_serialize_stream_open_body(uint8_t *buf, const struct frame_stream_open *open)
{
  if (!buf || !open) return -1;
  buf[0] = (uint8_t)((open->stream_id >> 8) & 0xFF);
  buf[1] = (uint8_t)(open->stream_id & 0xFF);
  buf[2] = ((open->reliable & 1) << 7)
         | ((open->ordered & 1) << 6)
         | (open->priority & 0x03);
  buf[3] = 0;
  return 4;
}

int frame_deserialize_stream_open_body(struct frame_stream_open *open, const uint8_t *buf)
{
  if (!open || !buf) return -1;
  open->stream_id = ((uint16_t)buf[0] << 8) | buf[1];
  open->reliable = (buf[2] >> 7) & 1;
  open->ordered = (buf[2] >> 6) & 1;
  open->priority = buf[2] & 0x03;
  return 4;
}

/* STREAM_CLOSE body is empty (just the frame header carries stream_id) */
int frame_serialize_stream_close_body(uint8_t *buf)
{
  (void)buf;
  return 0;
}

/* WINDOW_UPDATE body: max_data(4) */
int frame_serialize_window_update_body(uint8_t *buf, const struct frame_window_update *wu)
{
  if (!buf || !wu) return -1;
  uint32_t md = wu->max_data;
  buf[0] = (uint8_t)(md >> 24);
  buf[1] = (uint8_t)(md >> 16);
  buf[2] = (uint8_t)(md >> 8);
  buf[3] = (uint8_t)(md & 0xFF);
  return 4;
}

int frame_deserialize_window_update_body(struct frame_window_update *wu, const uint8_t *buf)
{
  if (!wu || !buf) return -1;
  wu->max_data = ((uint32_t)buf[0] << 24)
               | ((uint32_t)buf[1] << 16)
               | ((uint32_t)buf[2] << 8)
               | buf[3];
  return 4;
}

/* PING/PONG body: timestamp(8) */
int frame_serialize_ping_body(uint8_t *buf, uint64_t timestamp)
{
  if (!buf) return -1;
  buf[0] = (uint8_t)(timestamp >> 56);
  buf[1] = (uint8_t)(timestamp >> 48);
  buf[2] = (uint8_t)(timestamp >> 40);
  buf[3] = (uint8_t)(timestamp >> 32);
  buf[4] = (uint8_t)(timestamp >> 24);
  buf[5] = (uint8_t)(timestamp >> 16);
  buf[6] = (uint8_t)(timestamp >> 8);
  buf[7] = (uint8_t)(timestamp & 0xFF);
  return 8;
}

int frame_serialize_pong_body(uint8_t *buf, uint64_t timestamp)
{
  return frame_serialize_ping_body(buf, timestamp);
}

/* Coalescing: each frame is preceded by a 16-bit length field.
 * frame_get_total_length: verify a datagram and return total bytes consumed. */
int frame_get_total_length(const uint8_t *datagram, uint16_t dgram_len, int *frame_count)
{
  if (!datagram) return -1;
  uint16_t offset = 0;
  int count = 0;
  while (offset + FRAME_LENGTH_FIELD_SIZE <= dgram_len) {
    uint16_t flen = ((uint16_t)datagram[offset] << 8) | datagram[offset + 1];
    if (flen < FRAME_HEADER_SIZE) return -1;
    if (offset + FRAME_LENGTH_FIELD_SIZE + flen > dgram_len) return -1;
    offset += FRAME_LENGTH_FIELD_SIZE + flen;
    count++;
  }
  if (frame_count) *frame_count = count;
  return (int)offset;
}

/* Return the type of the first frame in a datagram */
int frame_peek_type(const uint8_t *datagram, uint16_t dgram_len)
{
  if (!datagram) return -1;
  if (dgram_len < FRAME_LENGTH_FIELD_SIZE + FRAME_HEADER_SIZE) return -1;
  uint16_t flen = ((uint16_t)datagram[0] << 8) | datagram[1];
  if (flen < FRAME_HEADER_SIZE) return -1;
  return datagram[2 + 1] & 0x3F;
}
