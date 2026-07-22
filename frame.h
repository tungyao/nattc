#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Maximum UDP datagram payload */
#define MAX_DATAGRAM_SIZE 1200

/* Protocol version */
#define FRAME_VERSION 0x01

/* Frame types */
#define FRAME_TYPE_DATA          0x01
#define FRAME_TYPE_ACK           0x02
#define FRAME_TYPE_FEC           0x03
#define FRAME_TYPE_STREAM_OPEN   0x04
#define FRAME_TYPE_STREAM_CLOSE  0x05
#define FRAME_TYPE_PING          0x06
#define FRAME_TYPE_PONG          0x07
#define FRAME_TYPE_WINDOW_UPDATE 0x08

/* Stream ID 0 = connection-level frames */
#define CONNECTION_STREAM_ID 0

/* Stream ID allocation */
#define STREAM_ID_INITIATOR_BASE 1
#define STREAM_ID_RESPONDER_BASE 2
#define STREAM_ID_STEP 2
#define stream_id_is_initiator(id) (((id) & 1) != 0)
#define stream_id_is_responder(id) (((id) & 1) == 0 && (id) != 0)

/* Per-frame header size (4 bytes) */
#define FRAME_HEADER_SIZE 4

/* Coalescing per-frame length field size */
#define FRAME_LENGTH_FIELD_SIZE 2

/* Per-frame header (4 bytes) */
struct frame_header {
  uint8_t version;
  uint8_t priority;
  uint8_t reserved;
  uint8_t type;
  uint16_t stream_id;
};

/* Maximum body we can fit in a frame (after header, in one datagram) */
#define MAX_FRAME_BODY (MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE - FRAME_LENGTH_FIELD_SIZE)

/* DATA frame body (after header) */
struct frame_data {
  uint32_t packet_seq;
  uint32_t stream_seq;
  uint16_t data_len;
};

/* Size of DATA frame header (fixed part before payload) */
#define FRAME_DATA_HEADER_SIZE (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t))

/* FEC frame body (after header) */
struct frame_fec {
  uint32_t group_id;
  uint16_t fec_index;
  uint16_t data_len;
};

#define FRAME_FEC_BODY_SIZE (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t))

/* ACK range entry */
struct ack_range {
  uint16_t gap;
  uint16_t length;
};

/* Fixed size of ACK frame before variable ranges */
#define FRAME_ACK_FIXED_SIZE (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t))

/* ACK frame body (after header) */
struct frame_ack {
  uint32_t largest_acked;
  uint16_t ack_delay;
  uint16_t first_ack_range;
  uint16_t range_count;
};

/* STREAM_OPEN frame body (after header) - 4 bytes */
struct frame_stream_open {
  uint16_t stream_id;
  uint8_t reliable;
  uint8_t ordered;
  uint8_t priority;
};

/* WINDOW_UPDATE frame body (after header) */
struct frame_window_update {
  uint32_t max_data;
};

/* Serialize/deserialize frame header */
int frame_serialize_header(uint8_t *buf, const struct frame_header *hdr);
int frame_deserialize_header(struct frame_header *hdr, const uint8_t *buf);

/* Serialize/deserialize DATA frame body (payload follows the fixed header) */
int frame_serialize_data_body(uint8_t *buf, const struct frame_data *data);
int frame_deserialize_data_body(struct frame_data *data, const uint8_t *buf);

/* Serialize/deserialize FEC frame body */
int frame_serialize_fec_body(uint8_t *buf, const struct frame_fec *fec);
int frame_deserialize_fec_body(struct frame_fec *fec, const uint8_t *buf);

/* Serialize/deserialize ACK frame body with variable ranges */
int frame_serialize_ack_body(uint8_t *buf, const struct frame_ack *ack,
                              const struct ack_range *ranges);
int frame_deserialize_ack_body(struct frame_ack *ack, struct ack_range *ranges,
                                int max_ranges, const uint8_t *buf, uint16_t buf_len);

/* Serialize/deserialize STREAM_OPEN frame body */
int frame_serialize_stream_open_body(uint8_t *buf, const struct frame_stream_open *open);
int frame_deserialize_stream_open_body(struct frame_stream_open *open, const uint8_t *buf);

/* Serialize STREAM_CLOSE frame body (just the 4-byte header, stream_id in header) */
int frame_serialize_stream_close_body(uint8_t *buf);

/* Serialize/deserialize WINDOW_UPDATE frame body */
int frame_serialize_window_update_body(uint8_t *buf, const struct frame_window_update *wu);
int frame_deserialize_window_update_body(struct frame_window_update *wu, const uint8_t *buf);

/* Serialize PING/PONG body (8-byte timestamp) */
int frame_serialize_ping_body(uint8_t *buf, uint64_t timestamp);
int frame_serialize_pong_body(uint8_t *buf, uint64_t timestamp);

/* Datagram coalescing helpers */
int frame_get_total_length(const uint8_t *datagram, uint16_t dgram_len, int *frame_count);
int frame_peek_type(const uint8_t *datagram, uint16_t dgram_len);

#endif /* FRAME_H */
