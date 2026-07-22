#include "reliable_udp.h"
#include "fec.h"
#include "congestion.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define WINDOW_UPDATE_FREE_RATIO 2

/* ------------------------------------------------------------------ */
/*  Q16.16 fixed-point helpers                                        */
/* ------------------------------------------------------------------ */
#define Q16_SHIFT 16
#define Q16_ONE (1 << Q16_SHIFT)
#define Q16_FROM_MS(ms) ((uint32_t)((ms) << Q16_SHIFT))
#define Q16_TO_MS(q16)  ((q16) >> Q16_SHIFT)
#define Q16_MUL(a, b)  ((uint32_t)(((uint64_t)(a) * (b)) >> Q16_SHIFT))
#define Q16_DIV(a, b)  ((uint32_t)(((uint64_t)(a) << Q16_SHIFT) / (b)))

/* ------------------------------------------------------------------ */
/*  Frame builder: coalesce frames into a datagram                    */
/* ------------------------------------------------------------------ */
struct frame_builder {
  uint8_t buf[MAX_DATAGRAM_SIZE];
  uint16_t offset;
};

static void fb_init(struct frame_builder *fb)
{
  fb->offset = 0;
}

static int fb_add(struct frame_builder *fb, const uint8_t *frame, uint16_t len)
{
  if (fb->offset + FRAME_LENGTH_FIELD_SIZE + len > MAX_DATAGRAM_SIZE)
    return -1;
  fb->buf[fb->offset++] = (uint8_t)(len >> 8);
  fb->buf[fb->offset++] = (uint8_t)(len & 0xFF);
  memcpy(fb->buf + fb->offset, frame, len);
  fb->offset += len;
  return 0;
}

static int fb_send(struct frame_builder *fb, struct reliable_conn *conn)
{
  if (fb->offset == 0) return 0;
  return conn->send_fn(conn->send_ctx, fb->buf, fb->offset,
                        (const struct sockaddr *)&conn->peer_addr,
                        sizeof(conn->peer_addr));
}

/* ------------------------------------------------------------------ */
/*  Helper: build a frame header into a buffer                        */
/* ------------------------------------------------------------------ */
static int build_header(uint8_t *buf, uint8_t type, uint16_t stream_id, uint8_t priority)
{
  struct frame_header hdr;
  hdr.version = FRAME_VERSION;
  hdr.priority = priority;
  hdr.reserved = 0;
  hdr.type = type;
  hdr.stream_id = stream_id;
  return frame_serialize_header(buf, &hdr);
}

/* ------------------------------------------------------------------ */
/*  In-flight packet table                                            */
/* ------------------------------------------------------------------ */
static struct inflight_entry* inflight_lookup(struct reliable_conn *conn,
                                               uint32_t packet_seq)
{
  for (int i = 0; i < MAX_INFLIGHT; i++) {
    struct inflight_entry *e = &conn->inflight[i];
    if (e->valid && e->packet_seq == packet_seq)
      return e;
  }
  return NULL;
}

static struct inflight_entry* inflight_insert(struct reliable_conn *conn,
                                               uint32_t packet_seq)
{
  /* Try indexed slot first */
  uint32_t idx = packet_seq % MAX_INFLIGHT;
  struct inflight_entry *first = &conn->inflight[idx];
  if (!first->valid || first->acked) {
    memset(first, 0, sizeof(*first));
    return first;
  }

  /* Fallback: reuse acked or invalid slots */
  struct inflight_entry *candidate = NULL;
  uint32_t oldest_time = (uint32_t)-1;

  for (int i = 0; i < MAX_INFLIGHT; i++) {
    struct inflight_entry *e = &conn->inflight[i];
    if (!e->valid)
      return memset(e, 0, sizeof(*e)), e;
    if (e->acked)
      return memset(e, 0, sizeof(*e)), e;
    if (e->send_time_ms < oldest_time) {
      oldest_time = e->send_time_ms;
      candidate = e;
    }
  }

  /* Evict oldest */
  if (candidate) {
    memset(candidate, 0, sizeof(*candidate));
    return candidate;
  }
  return NULL;
}

/* Mark all inflight entries for a stream as invalid (stream closing) */
static void inflight_clear_stream(struct reliable_conn *conn, uint16_t stream_id)
{
  for (int i = 0; i < MAX_INFLIGHT; i++) {
    struct inflight_entry *e = &conn->inflight[i];
    if (e->valid && e->stream_id == stream_id)
      e->valid = 0;
  }
}

/* ------------------------------------------------------------------ */
/*  RTT estimation (Q16.16)                                           */
/* ------------------------------------------------------------------ */
static void rtt_init(struct rtt_estimator *rtt)
{
  uint32_t initial_ms = INITIAL_RTT_MS;
  rtt->min_rtt = initial_ms;
  rtt->smoothed_rtt = Q16_FROM_MS(initial_ms);
  rtt->rtt_variance = Q16_FROM_MS(initial_ms) >> 1;
  rtt->latest_rtt = initial_ms;
}

static void rtt_update(struct rtt_estimator *rtt, uint32_t rtt_sample_ms)
{
  if (rtt_sample_ms < rtt->min_rtt)
    rtt->min_rtt = rtt_sample_ms;
  rtt->latest_rtt = rtt_sample_ms;

  uint32_t sample_q16 = Q16_FROM_MS(rtt_sample_ms);

  if (rtt->smoothed_rtt == 0) {
    rtt->smoothed_rtt = sample_q16;
    rtt->rtt_variance = sample_q16 >> 1;
  } else {
    uint32_t deviation;
    if (sample_q16 > rtt->smoothed_rtt)
      deviation = sample_q16 - rtt->smoothed_rtt;
    else
      deviation = rtt->smoothed_rtt - sample_q16;

    /* rtt_variance = 3/4 * rtt_variance + 1/4 * deviation */
    rtt->rtt_variance = rtt->rtt_variance - (rtt->rtt_variance >> 2)
                       + (deviation >> 2);
    /* smoothed_rtt = 7/8 * smoothed_rtt + 1/8 * sample */
    rtt->smoothed_rtt = rtt->smoothed_rtt - (rtt->smoothed_rtt >> 3)
                       + (sample_q16 >> 3);
  }
}

static uint32_t rtt_compute_timeout_ms(struct rtt_estimator *rtt,
                                        uint32_t max_ack_delay_ms)
{
  uint32_t smoothed_ms = Q16_TO_MS(rtt->smoothed_rtt);
  uint32_t variance_ms = Q16_TO_MS(rtt->rtt_variance);
  uint32_t t = smoothed_ms + 4 * variance_ms + max_ack_delay_ms;
  /* Minimum 10ms timeout */
  return t < 10 ? 10 : t;
}

/* ------------------------------------------------------------------ */
/*  ACK tracker (receive-side)                                        */
/* ------------------------------------------------------------------ */
static void ack_tracker_init(struct ack_tracker *at)
{
  at->base_seq = 0;
  at->max_seq = 0;
  at->last_received_time = 0;
  at->received_count = 0;
  memset(at->bitmap, 0, PACKET_HISTORY_SIZE);
}

static void ack_tracker_received(struct ack_tracker *at, uint32_t seq,
                                  uint32_t now_ms)
{
  if (seq >= at->base_seq + PACKET_HISTORY_BITS) {
    memset(at->bitmap, 0, PACKET_HISTORY_SIZE);
    at->base_seq = seq;
  }

  if (seq < at->base_seq)
    return;

  uint32_t bit_idx = seq - at->base_seq;
  at->bitmap[bit_idx >> 3] |= (uint8_t)(1 << (bit_idx & 7));

  if (at->received_count == 0 || seq > at->max_seq) {
    at->max_seq = seq;
    at->last_received_time = now_ms;
  }
  at->received_count++;
}

/* Generate ACK ranges from the tracker.
 * Returns the number of bytes written to ack_body, or -1 on error. */
static int ack_tracker_generate(struct ack_tracker *at, uint32_t now_ms,
                                 uint8_t *ack_body, uint16_t ack_body_cap)
{
  if (at->received_count == 0)
    return -1;

  uint32_t largest = at->max_seq;
  uint16_t first_range = 0;

  /* Count consecutive received ending at largest */
  for (uint32_t s = largest; ; s--) {
    if (s < at->base_seq) break;
    uint32_t idx = s - at->base_seq;
    if (!(at->bitmap[idx >> 3] & (uint8_t)(1 << (idx & 7)))) break;
    first_range++;
    if (s == 0) break;
  }

  /* Compute ack_delay = time since largest acked packet was received */
  uint16_t ack_delay = 0;
  if (at->last_received_time > 0 && now_ms > at->last_received_time) {
    uint32_t d = now_ms - at->last_received_time;
    ack_delay = d < 65535 ? (uint16_t)d : 65535;
  }

  struct frame_ack ack;
  ack.largest_acked = largest;
  ack.ack_delay = ack_delay;
  ack.first_ack_range = first_range;

  struct ack_range ranges[MAX_ACK_RANGES];
  int range_count = 0;

  if (first_range > largest) {
    /* All packets from 0 to largest are received — no ranges needed */
    ack.range_count = 0;
    int ret = frame_serialize_ack_body(ack_body, &ack, NULL);
    if (ret < 0 || (uint16_t)ret > ack_body_cap) return -1;
    return ret;
  }

  /* Walk backward from end of first range */
  uint32_t current = largest - first_range;
  while (current >= at->base_seq && range_count < MAX_ACK_RANGES) {
    /* Count gap (consecutive NOT received) */
    uint16_t gap = 0;
    while (1) {
      if (current < at->base_seq) break;
      uint32_t idx = current - at->base_seq;
      if (at->bitmap[idx >> 3] & (uint8_t)(1 << (idx & 7))) break;
      gap++;
      if (current == 0) break;
      current--;
    }
    if (gap == 0) break;

    /* Count run (consecutive received) */
    uint16_t run = 0;
    while (1) {
      if (current < at->base_seq) break;
      uint32_t idx = current - at->base_seq;
      if (!(at->bitmap[idx >> 3] & (uint8_t)(1 << (idx & 7)))) break;
      run++;
      if (current == 0) break;
      current--;
    }
    if (run == 0) break;

    ranges[range_count].gap = gap;
    ranges[range_count].length = run;
    range_count++;
  }

  ack.range_count = (uint16_t)range_count;

  int ret = frame_serialize_ack_body(ack_body, &ack, ranges);
  if (ret < 0 || (uint16_t)ret > ack_body_cap) return -1;
  return ret;
}

/* ------------------------------------------------------------------ */
/*  Stream helper                                                      */
/* ------------------------------------------------------------------ */
struct reliable_stream* find_stream(struct reliable_conn *conn,
                                             uint16_t stream_id)
{
  for (struct reliable_stream *s = conn->streams; s; s = s->next)
    if (s->stream_id == stream_id) return s;
  return NULL;
}

/* Free all pending recv nodes for a stream */
static void free_recv_pending(struct reliable_stream *stream)
{
  struct recv_data_node *n = stream->recv_pending;
  while (n) {
    struct recv_data_node *next = n->next;
    free(n);
    n = next;
  }
  stream->recv_pending = NULL;
}

/* Insert out-of-order data into the stream's recv_pending list.
 * List is sorted by stream_seq ascending. */
static int insert_recv_pending(struct reliable_stream *stream,
                                 uint32_t stream_seq,
                                 const uint8_t *data, uint32_t data_len)
{
  struct recv_data_node *node = (struct recv_data_node *)
    malloc(sizeof(struct recv_data_node) + data_len);
  if (!node) return -1;
  node->stream_seq = stream_seq;
  node->len = data_len;
  node->data = (uint8_t *)(node + 1);
  memcpy(node->data, data, data_len);
  node->next = NULL;

  /* Insert in sorted order */
  struct recv_data_node **pp = &stream->recv_pending;
  while (*pp && (*pp)->stream_seq < stream_seq)
    pp = &(*pp)->next;

  /* Check for duplicate */
  if (*pp && (*pp)->stream_seq == stream_seq) {
    free(node);
    return 0;
  }

  node->next = *pp;
  *pp = node;
  return 0;
}

/* Drain contiguous data from recv_pending into recv_buf.
 * Advances next_stream_seq_recv. */
static void drain_recv_pending(struct reliable_stream *stream)
{
  while (stream->recv_pending) {
    struct recv_data_node *n = stream->recv_pending;
    if (n->stream_seq != stream->next_stream_seq_recv)
      break;

    uint32_t space = RELIABLE_STREAM_RECV_BUF_SIZE
                   - (stream->recv_buf_head - stream->recv_buf_tail);
    if (space < n->len)
      break; /* recv buffer full */

    /* Copy into ring buffer */
    uint32_t head = stream->recv_buf_head % RELIABLE_STREAM_RECV_BUF_SIZE;
    if (head + n->len <= RELIABLE_STREAM_RECV_BUF_SIZE) {
      memcpy(stream->recv_buf + head, n->data, n->len);
    } else {
      uint32_t first = RELIABLE_STREAM_RECV_BUF_SIZE - head;
      memcpy(stream->recv_buf + head, n->data, first);
      memcpy(stream->recv_buf, n->data + first, n->len - first);
    }

    stream->recv_buf_head += n->len;
    stream->next_stream_seq_recv++;
    stream->recv_pending = n->next;
    free(n);
  }
}

/* Free a stream: unlink from list, free recv_pending, free memory */
static void free_stream(struct reliable_conn *conn, struct reliable_stream *stream)
{
  free_recv_pending(stream);
  inflight_clear_stream(conn, stream->stream_id);
  struct reliable_stream **pp = &conn->streams;
  while (*pp) {
    if (*pp == stream) {
      *pp = stream->next;
      break;
    }
    pp = &(*pp)->next;
  }
  free(stream);
}

/* ------------------------------------------------------------------ */
/*  Send an ACK frame immediately                                     */
/* ------------------------------------------------------------------ */
static int send_ack_frame(struct reliable_conn *conn, uint32_t now_ms)
{
  uint8_t ack_buf[MAX_DATAGRAM_SIZE];
  int hdr_sz = build_header(ack_buf, FRAME_TYPE_ACK, CONNECTION_STREAM_ID, 0);
  if (hdr_sz < 0) return -1;

  int ack_body_len = ack_tracker_generate(&conn->ack_tx, now_ms,
                                           ack_buf + FRAME_HEADER_SIZE,
                                           MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE);
  int ret = 0;
  if (ack_body_len > 0) {
    uint16_t ack_total = FRAME_HEADER_SIZE + (uint16_t)ack_body_len;
    struct frame_builder fb;
    fb_init(&fb);
    if (fb_add(&fb, ack_buf, ack_total) == 0)
      ret = fb_send(&fb, conn);
  }
  conn->pending_ack = 0;
  conn->last_ack_send_ms = now_ms;
  return ret;
}

/* Forward declarations for FEC helpers (defined below) */
static int fec_track_recv_packet(struct reliable_conn *conn,
                                  uint32_t packet_seq,
                                  uint16_t stream_id,
                                  uint32_t stream_seq,
                                  const uint8_t *payload,
                                  uint16_t payload_len,
                                  uint32_t now_ms);
static int fec_try_decode(struct reliable_conn *conn, uint32_t now_ms);

/* ------------------------------------------------------------------ */
/*  Process a received DATA frame                                     */
/* ------------------------------------------------------------------ */
static int process_data_frame(struct reliable_conn *conn,
                               uint16_t stream_id,
                               const struct frame_data *data,
                               const uint8_t *payload, uint32_t now_ms)
{
  struct reliable_stream *stream = find_stream(conn, stream_id);
  if (!stream)
    return -1;

  /* Record packet in ACK tracker */
  ack_tracker_received(&conn->ack_tx, data->packet_seq, now_ms);

  stream->flow.bytes_received += data->data_len;

  /* Set pending ACK flag (will be sent in tick or coalesced) */
  conn->pending_ack = 1;

  /* Check for duplicate or out-of-order */
  if (data->stream_seq < stream->next_stream_seq_recv) {
    /* Duplicate - already received */
    return 0;
  }

  if (data->stream_seq > stream->next_stream_seq_recv) {
    /* Out of order - buffer and send immediate ACK */
    int ret = insert_recv_pending(stream, data->stream_seq,
                                  payload, data->data_len);
    send_ack_frame(conn, now_ms);
    return ret < 0 ? ret : 0;
  }

  /* FEC: track received data packet for forward error correction */
  fec_track_recv_packet(conn, data->packet_seq, stream_id,
                         data->stream_seq, payload, data->data_len, now_ms);

  /* In-order: write to recv buffer directly */
  uint32_t space = RELIABLE_STREAM_RECV_BUF_SIZE
                 - (stream->recv_buf_head - stream->recv_buf_tail);
  if (space < data->data_len)
    return -1; /* buffer full, drop */

  uint32_t head = stream->recv_buf_head % RELIABLE_STREAM_RECV_BUF_SIZE;
  if (head + data->data_len <= RELIABLE_STREAM_RECV_BUF_SIZE) {
    memcpy(stream->recv_buf + head, payload, data->data_len);
  } else {
    uint32_t first = RELIABLE_STREAM_RECV_BUF_SIZE - head;
    memcpy(stream->recv_buf + head, payload, first);
    memcpy(stream->recv_buf, payload + first, data->data_len - first);
  }

  stream->recv_buf_head += data->data_len;
  stream->next_stream_seq_recv++;

  /* Try to drain pending */
  drain_recv_pending(stream);

  /* Try FEC decode now that we have a new data packet */
  fec_try_decode(conn, now_ms);

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Process a received ACK frame                                      */
/* ------------------------------------------------------------------ */
static void handle_acked_packet(struct reliable_conn *conn,
                                 uint32_t packet_seq, uint32_t now_ms)
{
  struct inflight_entry *e = inflight_lookup(conn, packet_seq);
  if (!e || e->acked) return;

  e->acked = 1;

  /* Update RTT if this is a first transmission (not retransmit) */
  if (e->retransmits == 0) {
    uint32_t sample_ms = now_ms - e->send_time_ms;
    rtt_update(&conn->rtt, sample_ms);
  }

  /* Decrement bytes_in_flight for the stream and connection */
  struct reliable_stream *s = find_stream(conn, e->stream_id);
  if (s && s->flow.bytes_in_flight >= e->payload_len)
    s->flow.bytes_in_flight -= e->payload_len;
  if (conn->conn_bytes_in_flight >= e->payload_len)
    conn->conn_bytes_in_flight -= e->payload_len;

  /* Congestion control: cwnd growth (only if not in recovery) */
  if (!conn->cubic.in_recovery) {
    if (conn->cubic.cwnd < conn->cubic.ssthresh) {
      conn->cubic.cwnd += e->payload_len;
    } else {
      int32_t t_minus_K = (int32_t)(now_ms - conn->cubic.last_loss_time) - (int32_t)conn->cubic.k;
      int64_t tdk3 = (int64_t)t_minus_K * t_minus_K * t_minus_K;
      int64_t w_cubic = ((int64_t)tdk3 * (int64_t)conn->cubic.cubic_c) >> 16;
      w_cubic += (int64_t)conn->cubic.w_max;
      if (w_cubic < 0) w_cubic = 0;
      conn->cubic.cwnd = (uint32_t)w_cubic;
      if (conn->cubic.cwnd < CWND_MIN)
        conn->cubic.cwnd = CWND_MIN;
      if (conn->cubic.cwnd > conn->cubic.ssthresh)
        conn->cubic.cwnd = conn->cubic.ssthresh;
    }
  }

  /* Bandwidth estimation */
  conn->bytes_acked_since_last_estimate += e->payload_len;
  uint32_t smoothed_ms = Q16_TO_MS(conn->rtt.smoothed_rtt);
  if (smoothed_ms > 0 && now_ms - conn->last_bandwidth_estimate_ms >= smoothed_ms) {
    conn->delivery_rate = Q16_DIV(conn->bytes_acked_since_last_estimate, smoothed_ms);
    conn->bytes_acked_since_last_estimate = 0;
    conn->last_bandwidth_estimate_ms = now_ms;
  }
}

static void process_ack_frame(struct reliable_conn *conn,
                               struct frame_ack *ack,
                               struct ack_range *ranges,
                               uint32_t now_ms)
{
  uint32_t current = ack->largest_acked;

  /* Build bitmap of acked packets for this ACK */
  uint8_t acked_bitmap[PACKET_HISTORY_SIZE];
  memset(acked_bitmap, 0, PACKET_HISTORY_SIZE);
  uint32_t acked_base = ack->largest_acked;

  /* Process first range (consecutive from largest) */
  for (uint16_t i = 0; i < ack->first_ack_range; i++) {
    uint32_t bit_idx = acked_base - current;
    if (bit_idx < PACKET_HISTORY_BITS)
      acked_bitmap[bit_idx >> 3] |= (uint8_t)(1 << (bit_idx & 7));
    handle_acked_packet(conn, current, now_ms);
    if (current == 0) { current = 0; break; }
    current--;
  }

  /* Process additional ranges */
  for (uint16_t i = 0; i < ack->range_count; i++) {
    if (current < ranges[i].gap) break;
    current -= ranges[i].gap;

    for (uint16_t j = 0; j < ranges[i].length; j++) {
      uint32_t bit_idx = acked_base - current;
      if (bit_idx < PACKET_HISTORY_BITS)
        acked_bitmap[bit_idx >> 3] |= (uint8_t)(1 << (bit_idx & 7));
      handle_acked_packet(conn, current, now_ms);
      if (current == 0) break;
      current--;
    }
    if (current == 0) break;
  }

  /* Fast retransmit: increment dup_ack_count for unacked packets < largest_acked */
  if (ack->largest_acked > 0) {
    for (int i = 0; i < MAX_INFLIGHT; i++) {
      struct inflight_entry *e = &conn->inflight[i];
      if (!e->valid || e->acked || !e->reliable) continue;
      if (e->packet_seq >= ack->largest_acked) continue;

      uint32_t bit_idx = acked_base - e->packet_seq;
      int is_acked = (bit_idx < PACKET_HISTORY_BITS) &&
                     (acked_bitmap[bit_idx >> 3] & (uint8_t)(1 << (bit_idx & 7)));

      if (!is_acked) {
        e->dup_ack_count++;
        if (e->dup_ack_count >= 3) {
          /* Fast retransmit */
          if (!conn->cubic.in_recovery)
            cubic_on_loss(&conn->cubic, now_ms);
          e->retransmits++;
          conn->packets_lost++;
          conn->packets_retransmitted++;
          conn->fec_loss_counter++;

          uint32_t new_seq = conn->next_packet_seq++;
          uint8_t frame[MAX_DATAGRAM_SIZE];
          uint8_t retrans_prio = 0;
          struct reliable_stream *retrans_s = find_stream(conn, e->stream_id);
          if (retrans_s) retrans_prio = retrans_s->priority;
          build_header(frame, FRAME_TYPE_DATA, e->stream_id, retrans_prio);

          struct frame_data fd;
          fd.packet_seq = new_seq;
          fd.stream_seq = e->stream_seq;
          fd.data_len = e->payload_len;
          int body_off = frame_serialize_data_body(frame + FRAME_HEADER_SIZE, &fd);
          memcpy(frame + FRAME_HEADER_SIZE + body_off, e->payload, e->payload_len);

          uint16_t frame_len = FRAME_HEADER_SIZE + body_off + e->payload_len;

          e->packet_seq = new_seq;
          e->send_time_ms = now_ms;
          e->acked = 0;
          e->dup_ack_count = 0;

          struct frame_builder rt_fb;
          fb_init(&rt_fb);
          if (fb_add(&rt_fb, frame, frame_len) == 0)
            conn->send_fn(conn->send_ctx, rt_fb.buf, rt_fb.offset,
                          (const struct sockaddr *)&conn->peer_addr,
                          sizeof(conn->peer_addr));
        }
      }
    }
  }

  /* Remove acked entries from inflight table */
  for (int i = 0; i < MAX_INFLIGHT; i++) {
    struct inflight_entry *e = &conn->inflight[i];
    if (e->valid && e->acked)
      e->valid = 0;
  }

  /* Recovery exit check: clear in_recovery after 1 RTT with no loss */
  if (conn->cubic.in_recovery) {
    uint32_t rtt_ms = Q16_TO_MS(conn->rtt.smoothed_rtt);
    if (now_ms - conn->cubic.last_loss_time > rtt_ms)
      conn->cubic.in_recovery = 0;
  }

  /* Delay monitor */
  {
    uint32_t current_rtt_ms = conn->rtt.latest_rtt;
    uint32_t current_rtt_q16 = Q16_FROM_MS(current_rtt_ms);
    if (current_rtt_q16 > conn->delay.rtt_threshold &&
        !conn->delay.delay_reduced) {
      conn->cubic.cwnd = (uint32_t)((uint64_t)conn->cubic.cwnd * 55705 >> 16);
      if (conn->cubic.cwnd < CWND_MIN)
        conn->cubic.cwnd = CWND_MIN;
      conn->delay.delay_reduced = 1;
      conn->delay.last_delay_reduce = now_ms;
    }
    if (conn->delay.delay_reduced) {
      uint32_t rtt_ms = Q16_TO_MS(conn->rtt.smoothed_rtt);
      if (now_ms - conn->delay.last_delay_reduce > rtt_ms)
        conn->delay.delay_reduced = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  FEC helpers                                                       */
/* ------------------------------------------------------------------ */

/* Compute group_id for a packet_seq given fec_n */
static uint32_t fec_group_id(uint32_t packet_seq, uint8_t fec_n)
{
  if (fec_n == 0) return 0;
  return packet_seq / fec_n;
}

/* Compute fec_index for a packet_seq given fec_n */
static uint8_t fec_index_from_seq(uint32_t packet_seq, uint8_t fec_n)
{
  if (fec_n == 0) return 0;
  return (uint8_t)(packet_seq % fec_n);
}

/* Track a sent DATA packet in the FEC send group.
 * Returns 1 if group is complete and FEC frames should be sent, 0 otherwise. */
static int fec_track_sent_packet(struct reliable_conn *conn,
                                  uint32_t packet_seq,
                                  uint16_t stream_id,
                                  uint32_t stream_seq,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
  if (!conn->fec_enabled || conn->fec_n == 0) return 0;

  uint8_t idx = conn->fec_send.count;
  conn->fec_send.packet_seqs[idx] = packet_seq;
  conn->fec_send.stream_ids[idx] = stream_id;
  conn->fec_send.stream_seqs[idx] = stream_seq;
  conn->fec_send.data_lengths[idx] = payload_len;
  conn->fec_send.packets[idx].len = payload_len;
  if (payload_len > 0)
    memcpy(conn->fec_send.packets[idx].data, payload, payload_len);
  conn->fec_send.count++;

  if (conn->fec_send.count >= conn->fec_n) {
    conn->fec_send.group_id = fec_group_id(packet_seq, conn->fec_n);
    conn->fec_send.count = 0;
    return 1;
  }
  return 0;
}

/* Encode and send FEC frames for the completed send group */
static int fec_send_parity(struct reliable_conn *conn,
                            struct frame_builder *fb,
                            uint32_t now_ms)
{
  struct fec_ctx ctx;
  fec_init(&ctx, conn->fec_n, conn->fec_m);

  uint16_t padded_len = fec_padded_length(conn->fec_send.packets, conn->fec_n);
  if (padded_len == 0) return 0;
  uint16_t max_padded = MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE - FRAME_FEC_BODY_SIZE;
  if (padded_len > max_padded) padded_len = max_padded;

  struct fec_packet parity[FEC_MAX_M];
  memset(parity, 0, sizeof(parity));

  if (fec_encode(&ctx, conn->fec_send.packets, parity, padded_len) != 0)
    return -1;

  uint32_t group_id = conn->fec_send.group_id;

  for (uint8_t i = 0; i < conn->fec_m; i++) {
    uint8_t frame[MAX_DATAGRAM_SIZE];
    build_header(frame, FRAME_TYPE_FEC, CONNECTION_STREAM_ID, 0);

    struct frame_fec fec_hdr;
    fec_hdr.group_id = group_id;
    fec_hdr.fec_index = conn->fec_n + i;
    fec_hdr.data_len = padded_len;

    int body_off = frame_serialize_fec_body(frame + FRAME_HEADER_SIZE, &fec_hdr);
    if (body_off < 0) return -1;

    memcpy(frame + FRAME_HEADER_SIZE + body_off, parity[i].data, padded_len);
    uint16_t frame_len = FRAME_HEADER_SIZE + (uint16_t)body_off + padded_len;

    if (frame_len > MAX_DATAGRAM_SIZE) return -1;

    if (fb_add(fb, frame, frame_len) != 0) {
      if (fb_send(fb, conn) < 0) return -1;
      fb_init(fb);
      if (fb_add(fb, frame, frame_len) != 0) return -1;
    }
  }

  fec_free(&ctx);
  return 0;
}

/* Process a received FEC parity frame */
static int process_fec_frame(struct reliable_conn *conn,
                              const struct frame_fec *fec,
                              const uint8_t *payload,
                              uint32_t now_ms)
{
  (void)now_ms;
  if (!conn->fec_enabled) return 0;

  uint8_t n = conn->fec_n;
  uint8_t m = conn->fec_m;
  uint32_t group_id = fec->group_id;
  uint8_t fec_idx = (uint8_t)fec->fec_index;
  uint16_t data_len = fec->data_len;

  if (n == 0 || m == 0) return 0;
  if (fec_idx < n || fec_idx >= n + m) return 0;
  if (data_len > FEC_MAX_PACKET_SIZE) return 0;

  struct fec_recv_group *rg = &conn->fec_recv;

  /* If starting a new group or group changed, reset if old group is
   * stale. We only track one group at a time. */
  if (!rg->active || rg->group_id != group_id) {
    /* If we have a previously active group with enough packets, try to decode it */
    if (rg->active && rg->received_count >= n) {
      /* Try to decode old group later on next DATA frame arrival */
    }
    memset(rg, 0, sizeof(*rg));
    rg->active = 1;
    rg->group_id = group_id;
  }

  /* Store FEC parity packet */
  uint8_t idx = fec_idx;
  if (idx < FEC_MAX_GROUP_SIZE) {
    if (!(rg->received_bitmask & ((uint64_t)1 << idx))) {
      uint16_t copy_len = data_len < FEC_MAX_PACKET_SIZE ? data_len : FEC_MAX_PACKET_SIZE;
      memcpy(rg->packets[idx].data, payload, copy_len);
      rg->packets[idx].len = copy_len;
      rg->data_lengths[idx] = copy_len;
      rg->received_bitmask |= ((uint64_t)1 << idx);
      rg->received_count++;
    }
  }

  return 0;
}

/* Attempt FEC decode on the current receive group if enough packets received */
static int fec_try_decode(struct reliable_conn *conn, uint32_t now_ms)
{
  struct fec_recv_group *rg = &conn->fec_recv;
  if (!rg->active) return 0;
  if (rg->received_count < conn->fec_n) return 0;

  /* Check if all data indices (0..fec_n-1) have been received */
  uint64_t data_mask = ((uint64_t)1 << conn->fec_n) - 1;
  uint64_t recv_data = rg->received_bitmask & data_mask;
  if (recv_data == data_mask) {
    /* All data packets received - no decode needed */
    rg->active = 0;
    return 0;
  }

  /* Some data packets are missing. Count how many are missing. */
  uint8_t missing_count = 0;
  uint8_t missing_indices[FEC_MAX_M];
  uint8_t total = conn->fec_n + conn->fec_m;

  for (uint8_t i = 0; i < total && missing_count <= conn->fec_m; i++) {
    if (!(rg->received_bitmask & ((uint64_t)1 << i))) {
      missing_indices[missing_count++] = i;
    }
  }

  if (missing_count > conn->fec_m) {
    /* Too many losses, can't recover */
    rg->active = 0;
    return -1;
  }

  /* Build the packet array for decode (N+M entries) */
  struct fec_packet decode_packets[FEC_MAX_GROUP_SIZE];
  uint16_t padded_len = 0;

  /* Find padded length from received packets */
  for (uint8_t i = 0; i < total; i++) {
    if (rg->received_bitmask & ((uint64_t)1 << i)) {
      if (rg->data_lengths[i] > padded_len)
        padded_len = rg->data_lengths[i];
    }
  }
  if (padded_len == 0) { rg->active = 0; return -1; }
  if (padded_len > FEC_MAX_PACKET_SIZE) padded_len = FEC_MAX_PACKET_SIZE;

  /* Fill decode array */
  for (uint8_t i = 0; i < total; i++) {
    if (rg->received_bitmask & ((uint64_t)1 << i)) {
      memcpy(decode_packets[i].data, rg->packets[i].data, rg->data_lengths[i]);
      decode_packets[i].len = rg->data_lengths[i];
    } else {
      memset(decode_packets[i].data, 0, padded_len);
      decode_packets[i].len = 0;
    }
  }

  struct fec_ctx ctx;
  fec_init(&ctx, conn->fec_n, conn->fec_m);

  int ret = fec_decode(&ctx, decode_packets, missing_indices, missing_count, padded_len);

  if (ret == 0) {
    /* Deliver recovered data packets */
    for (uint8_t i = 0; i < conn->fec_n; i++) {
      if (!(rg->received_bitmask & ((uint64_t)1 << i))) {
        /* This packet was missing and is now recovered.
         * Deliver via process_data_frame by injecting a synthetic DATA frame. */
        uint32_t packet_seq = rg->packet_seqs[i];
        uint16_t stream_id = rg->stream_ids[i];
        uint32_t stream_seq = rg->stream_seqs[i];

        uint8_t frame_buf[MAX_DATAGRAM_SIZE];
        uint8_t *payload = frame_buf + FRAME_HEADER_SIZE + FRAME_DATA_HEADER_SIZE;

        build_header(frame_buf, FRAME_TYPE_DATA, stream_id, 0);

        struct frame_data fd;
        fd.packet_seq = packet_seq;
        fd.stream_seq = stream_seq;
        fd.data_len = padded_len;
        frame_serialize_data_body(frame_buf + FRAME_HEADER_SIZE, &fd);

        uint16_t copy_len = padded_len;
        if (copy_len > MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE - FRAME_DATA_HEADER_SIZE)
          copy_len = MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE - FRAME_DATA_HEADER_SIZE;
        memcpy(payload, decode_packets[i].data, copy_len);
        fd.data_len = copy_len;

        process_data_frame(conn, stream_id, &fd, payload, now_ms);
      }
    }
  }

  fec_free(&ctx);
  rg->active = 0;
  return 0;
}

/* Track a received DATA packet in the FEC receive group */
static int fec_track_recv_packet(struct reliable_conn *conn,
                                  uint32_t packet_seq,
                                  uint16_t stream_id,
                                  uint32_t stream_seq,
                                  const uint8_t *payload,
                                  uint16_t payload_len,
                                  uint32_t now_ms)
{
  if (!conn->fec_enabled) return 0;

  uint8_t n = conn->fec_n;
  if (n == 0) return 0;

  uint32_t group_id = fec_group_id(packet_seq, n);
  uint8_t idx = fec_index_from_seq(packet_seq, n);

  if (idx >= n) return 0;

  struct fec_recv_group *rg = &conn->fec_recv;

  if (!rg->active || rg->group_id != group_id) {
    /* Try decoding previous group if possible */
    if (rg->active && rg->received_count >= n)
      fec_try_decode(conn, now_ms);

    memset(rg, 0, sizeof(*rg));
    rg->active = 1;
    rg->group_id = group_id;
  }

  if (!(rg->received_bitmask & ((uint64_t)1 << idx))) {
    uint16_t copy_len = payload_len < FEC_MAX_PACKET_SIZE ? payload_len : FEC_MAX_PACKET_SIZE;
    memcpy(rg->packets[idx].data, payload, copy_len);
    rg->packets[idx].len = copy_len;
    rg->packet_seqs[idx] = packet_seq;
    rg->stream_ids[idx] = stream_id;
    rg->stream_seqs[idx] = stream_seq;
    rg->data_lengths[idx] = copy_len;
    rg->received_bitmask |= ((uint64_t)1 << idx);
    rg->received_count++;
  }

  return 0;
}

/* Adaptive FEC: adjust M based on loss rate every FEC_ADAPT_INTERVAL packets */
static void fec_adaptive_update(struct reliable_conn *conn)
{
  if (conn->fec_total_counter < FEC_ADAPT_INTERVAL) return;

  float loss_rate = (float)conn->fec_loss_counter / (float)conn->fec_total_counter;
  uint8_t new_m = fec_adaptive_m(loss_rate);
  if (new_m > FEC_MAX_M) new_m = FEC_MAX_M;
  if (new_m < 1) new_m = 1;
  conn->fec_m = new_m;
  conn->fec_loss_counter = 0;
  conn->fec_total_counter = 0;
}

/* ------------------------------------------------------------------ */
/*  Send pending data from a stream                                   */
/* ------------------------------------------------------------------ */
static int send_stream_data(struct reliable_conn *conn,
                             struct reliable_stream *stream,
                             struct frame_builder *fb,
                             uint32_t now_ms)
{
  uint32_t avail = stream->send_buf_head - stream->send_buf_tail;
  if (avail == 0) return 0;

  while (avail > 0) {
    /* Check stream-level flow control (absolute byte offset) */
    if (stream->reliable &&
        stream->total_bytes_sent >= stream->flow.send_window)
      break;

    /* Check connection-level flow control */
    if (conn->conn_bytes_in_flight >= conn->conn_send_window)
      break;

    uint32_t chunk_len = avail < MAX_FRAME_BODY ? avail : MAX_FRAME_BODY;

    /* Congestion control: don't exceed cwnd */
    if (conn->cubic.cwnd > conn->conn_bytes_in_flight) {
      uint32_t cwnd_rem = conn->cubic.cwnd - conn->conn_bytes_in_flight;
      if (chunk_len > cwnd_rem) chunk_len = cwnd_rem;
    } else {
      break;
    }

    /* Don't exceed remaining window (absolute offset limit) */
    if (stream->reliable) {
      if (stream->total_bytes_sent >= stream->flow.send_window) break;
      uint32_t stream_rem = stream->flow.send_window - stream->total_bytes_sent;
      if (chunk_len > stream_rem) chunk_len = stream_rem;
    }
    uint32_t conn_rem = conn->conn_send_window - conn->conn_bytes_in_flight;
    if (chunk_len > conn_rem) chunk_len = conn_rem;
    if (chunk_len == 0) break;

    uint32_t packet_seq = conn->next_packet_seq++;

    /* Build DATA frame */
    uint8_t frame[MAX_DATAGRAM_SIZE];
    build_header(frame, FRAME_TYPE_DATA, stream->stream_id, stream->priority);

    struct frame_data fd;
    fd.packet_seq = packet_seq;
    fd.stream_seq = stream->next_stream_seq_send++;
    fd.data_len = (uint16_t)chunk_len;
    int body_off = frame_serialize_data_body(frame + FRAME_HEADER_SIZE, &fd);

    /* Copy payload from stream send buffer */
    uint32_t tail = stream->send_buf_tail % RELIABLE_STREAM_SEND_BUF_SIZE;
    if (tail + chunk_len <= RELIABLE_STREAM_SEND_BUF_SIZE) {
      memcpy(frame + FRAME_HEADER_SIZE + body_off,
             stream->send_buf + tail, chunk_len);
    } else {
      uint32_t first = RELIABLE_STREAM_SEND_BUF_SIZE - tail;
      memcpy(frame + FRAME_HEADER_SIZE + body_off,
             stream->send_buf + tail, first);
      memcpy(frame + FRAME_HEADER_SIZE + body_off + first,
             stream->send_buf, chunk_len - first);
    }

    stream->total_bytes_sent += chunk_len;
    stream->send_buf_tail += chunk_len;
    avail -= chunk_len;

    uint16_t frame_len = (uint16_t)(FRAME_HEADER_SIZE + body_off + chunk_len);

    /* Track in inflight table */
    struct inflight_entry *ie = inflight_insert(conn, packet_seq);
    if (ie) {
      ie->packet_seq = packet_seq;
      ie->stream_seq = fd.stream_seq;
      ie->payload_len = (uint16_t)chunk_len;
      ie->send_time_ms = now_ms;
      ie->retransmits = 0;
      ie->stream_id = stream->stream_id;
      ie->reliable = stream->reliable;
      ie->valid = 1;
      ie->acked = 0;
      if (chunk_len > 0 && ie->reliable)
        memcpy(ie->payload, frame + FRAME_HEADER_SIZE + body_off, chunk_len);
    }

    conn->packets_sent++;
    conn->bytes_sent += chunk_len;

    /* Track bytes in flight for flow control */
    if (stream->reliable) {
      stream->flow.bytes_in_flight += chunk_len;
      conn->conn_bytes_in_flight += chunk_len;
    }

    /* Add to frame builder (will coalesce with other frames) */
    if (fb_add(fb, frame, frame_len) != 0) {
      /* Datagram full; send what we have, start new builder */
      if (fb_send(fb, conn) < 0) return -1;
      fb_init(fb);
      if (fb_add(fb, frame, frame_len) != 0) return -1;
    }

    /* FEC: track reliable data packets for forward error correction */
    if (stream->reliable && conn->fec_enabled) {
      const uint8_t *pload = frame + FRAME_HEADER_SIZE + body_off;
      if (fec_track_sent_packet(conn, packet_seq, stream->stream_id,
                                 fd.stream_seq, pload, (uint16_t)chunk_len)) {
        if (fec_send_parity(conn, fb, now_ms) != 0) return -1;
      }
      conn->fec_total_counter++;
      fec_adaptive_update(conn);
    }

    if (!stream->reliable) {
      /* Unreliable: don't retransmit */
      if (ie) ie->valid = 0;
    }
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Send a STREAM_CLOSE frame for a stream                            */
/* ------------------------------------------------------------------ */
static int send_stream_close_frame(struct reliable_stream *stream)
{
  struct reliable_conn *conn = stream->conn;
  if (!conn) return -1;

  uint8_t frame[MAX_DATAGRAM_SIZE];
  int hdr_sz = build_header(frame, FRAME_TYPE_STREAM_CLOSE, stream->stream_id, 0);
  if (hdr_sz < 0) return -1;

  frame_serialize_stream_close_body(frame + FRAME_HEADER_SIZE);
  uint16_t frame_len = FRAME_HEADER_SIZE;

  struct frame_builder fb;
  fb_init(&fb);
  if (fb_add(&fb, frame, frame_len) != 0) return -1;
  int ret = fb_send(&fb, conn);
  if (ret >= 0)
    stream->sent_close_frame = 1;
  return ret;
}

/* ------------------------------------------------------------------ */
/*  Send a WINDOW_UPDATE frame for a stream/connection                */
/* ------------------------------------------------------------------ */
static int send_window_update_frame(struct reliable_conn *conn,
                                     uint16_t stream_id, uint32_t max_data)
{
  uint8_t frame[MAX_DATAGRAM_SIZE];
  int hdr_sz = build_header(frame, FRAME_TYPE_WINDOW_UPDATE, stream_id, 0);
  if (hdr_sz < 0) return -1;

  struct frame_window_update wu;
  wu.max_data = max_data;
  int body_sz = frame_serialize_window_update_body(frame + FRAME_HEADER_SIZE, &wu);
  if (body_sz < 0) return -1;

  uint16_t frame_len = FRAME_HEADER_SIZE + (uint16_t)body_sz;

  struct frame_builder fb;
  fb_init(&fb);
  if (fb_add(&fb, frame, frame_len) != 0) return -1;
  return fb_send(&fb, conn);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

struct reliable_conn* reliable_conn_create(uint32_t session_id,
                                            send_func_t send_fn,
                                            void *send_ctx)
{
  struct reliable_conn *conn = (struct reliable_conn *)
    calloc(1, sizeof(struct reliable_conn));
  if (!conn) return NULL;

  conn->session_id = session_id;
  conn->send_fn = send_fn;
  conn->send_ctx = send_ctx;
  conn->next_packet_seq = 0;
  conn->max_ack_delay_ms = DEFAULT_MAX_ACK_DELAY_MS;
  conn->is_initiator = 0;
  conn->next_initiator_stream_id = STREAM_ID_INITIATOR_BASE;
  conn->next_responder_stream_id = STREAM_ID_RESPONDER_BASE;
  conn->conn_bytes_in_flight = 0;
  conn->conn_send_window = MAX_STREAMS * RELIABLE_STREAM_SEND_BUF_SIZE;
  conn->pending_ack = 0;
  conn->last_ack_send_ms = 0;
  conn->last_tick_ms = 0;

  rtt_init(&conn->rtt);
  ack_tracker_init(&conn->ack_tx);

  cubic_init(&conn->cubic, &conn->delay);
  conn->bytes_acked_since_last_estimate = 0;
  conn->last_bandwidth_estimate_ms = 0;
  conn->delivery_rate = 0;

  /* FEC initialization */
  conn->fec_enabled = 1;
  conn->fec_n = FEC_DEFAULT_N;
  conn->fec_m = FEC_DEFAULT_M;
  conn->fec_loss_counter = 0;
  conn->fec_total_counter = 0;
  memset(&conn->fec_send, 0, sizeof(conn->fec_send));
  memset(&conn->fec_recv, 0, sizeof(conn->fec_recv));

  memset(&conn->peer_addr, 0, sizeof(conn->peer_addr));

  return conn;
}

void reliable_conn_destroy(struct reliable_conn *conn)
{
  if (!conn) return;

  /* Free all streams */
  struct reliable_stream *s = conn->streams;
  while (s) {
    struct reliable_stream *next = s->next;
    free_recv_pending(s);
    free(s);
    s = next;
  }

  free(conn);
}

void reliable_conn_set_peer(struct reliable_conn *conn,
                             const struct sockaddr_in *addr)
{
  if (conn && addr)
    memcpy(&conn->peer_addr, addr, sizeof(conn->peer_addr));
}

struct reliable_stream* reliable_stream_open(struct reliable_conn *conn,
                                              uint8_t reliable,
                                              uint8_t ordered,
                                              uint8_t priority)
{
  if (!conn) return NULL;

  /* Enforce MAX_STREAMS limit */
  uint32_t stream_count = 0;
  for (struct reliable_stream *s = conn->streams; s; s = s->next) {
    if (++stream_count >= MAX_STREAMS) return NULL;
  }

  struct reliable_stream *stream = (struct reliable_stream *)
    calloc(1, sizeof(struct reliable_stream));
  if (!stream) return NULL;

  uint16_t sid;
  if (conn->is_initiator) {
    sid = conn->next_initiator_stream_id;
    conn->next_initiator_stream_id += STREAM_ID_STEP;
  } else {
    sid = conn->next_responder_stream_id;
    conn->next_responder_stream_id += STREAM_ID_STEP;
  }

  if (priority > 3) priority = 3;

  stream->stream_id = sid;
  stream->reliable = reliable;
  stream->ordered = ordered;
  stream->priority = priority;
  stream->total_bytes_sent = 0;
  stream->pending_window_update = 0;
  stream->state = STREAM_OPEN;
  stream->sent_close_frame = 0;
  stream->remote_closed = 0;
  stream->conn = conn;

  stream->next_stream_seq_send = 1;
  stream->next_stream_seq_recv = 1;
  stream->recv_pending = NULL;

  /* Default flow control windows (64KB each) */
  stream->flow.send_window = RELIABLE_STREAM_SEND_BUF_SIZE;
  stream->flow.recv_window = RELIABLE_STREAM_RECV_BUF_SIZE;
  stream->flow.bytes_in_flight = 0;
  stream->flow.bytes_received = 0;

  /* Insert into connection's stream list */
  stream->next = conn->streams;
  conn->streams = stream;

  return stream;
}

void reliable_stream_close(struct reliable_stream *stream)
{
  if (!stream) return;

  if (stream->state == STREAM_CLOSED) return;

  stream->state = STREAM_SEND_CLOSED;

  struct reliable_conn *conn = stream->conn;

  /* Send STREAM_CLOSE frame to peer (if not already sent) */
  if (conn && !stream->sent_close_frame) {
    send_stream_close_frame(stream);
  }

  /* If remote already closed, or no pending data, fully close */
  int can_close = (stream->send_buf_head == stream->send_buf_tail);
  if (stream->remote_closed || can_close) {
    stream->state = STREAM_CLOSED;
    if (conn)
      free_stream(conn, stream);
  }
}

int reliable_stream_send(struct reliable_stream *stream,
                          const void *data, uint32_t len)
{
  if (!stream || !data || stream->state != STREAM_OPEN) return -1;
  if (len == 0) return 0;

  uint32_t avail = RELIABLE_STREAM_SEND_BUF_SIZE
                 - (stream->send_buf_head - stream->send_buf_tail);
  if (avail < len) return -1;

  uint32_t head = stream->send_buf_head % RELIABLE_STREAM_SEND_BUF_SIZE;
  if (head + len <= RELIABLE_STREAM_SEND_BUF_SIZE) {
    memcpy(stream->send_buf + head, data, len);
  } else {
    uint32_t first = RELIABLE_STREAM_SEND_BUF_SIZE - head;
    memcpy(stream->send_buf + head, data, first);
    memcpy(stream->send_buf, (const uint8_t *)data + first, len - first);
  }

  stream->send_buf_head += len;
  return (int)len;
}

int reliable_stream_recv(struct reliable_stream *stream,
                          void *buf, uint32_t buf_len)
{
  if (!stream || !buf || buf_len == 0) return -1;

  uint32_t avail = stream->recv_buf_head - stream->recv_buf_tail;
  if (avail == 0) return -2; /* no data */

  uint32_t to_copy = avail < buf_len ? avail : buf_len;

  uint32_t tail = stream->recv_buf_tail % RELIABLE_STREAM_RECV_BUF_SIZE;
  if (tail + to_copy <= RELIABLE_STREAM_RECV_BUF_SIZE) {
    memcpy(buf, stream->recv_buf + tail, to_copy);
  } else {
    uint32_t first = RELIABLE_STREAM_RECV_BUF_SIZE - tail;
    memcpy(buf, stream->recv_buf + tail, first);
    memcpy((uint8_t *)buf + first, stream->recv_buf, to_copy - first);
  }

  stream->recv_buf_tail += to_copy;

  /* Proactive WINDOW_UPDATE: if recv buffer has opened up significantly */
  struct reliable_conn *conn = stream->conn;
  if (conn) {
    uint32_t recv_used = stream->recv_buf_head - stream->recv_buf_tail;
    uint32_t recv_free = RELIABLE_STREAM_RECV_BUF_SIZE - recv_used;
    /* Send update if free space > 50% of buffer */
    if (recv_free > RELIABLE_STREAM_RECV_BUF_SIZE / WINDOW_UPDATE_FREE_RATIO) {
      uint32_t max_data = stream->recv_buf_tail + RELIABLE_STREAM_RECV_BUF_SIZE;
      if (send_window_update_frame(conn, stream->stream_id, max_data) < 0)
        stream->pending_window_update = 1;
    }
  }

  return (int)to_copy;
}

int reliable_conn_tick(struct reliable_conn *conn, uint32_t now_ms)
{
  if (!conn) return -1;

  struct frame_builder fb;
  fb_init(&fb);

  conn->last_tick_ms = now_ms;

  /* 0. Retry any pending WINDOW_UPDATE frames */
  {
    struct reliable_stream *wu_s = conn->streams;
    while (wu_s) {
      if (wu_s->pending_window_update) {
        uint32_t max_data = wu_s->recv_buf_tail + RELIABLE_STREAM_RECV_BUF_SIZE;
        if (send_window_update_frame(conn, wu_s->stream_id, max_data) == 0)
          wu_s->pending_window_update = 0;
      }
      wu_s = wu_s->next;
    }
  }

  /* 1. Check for retransmission timeouts */
  uint32_t timeout_ms = rtt_compute_timeout_ms(&conn->rtt,
                                                 conn->max_ack_delay_ms);
  for (int i = 0; i < MAX_INFLIGHT; i++) {
    struct inflight_entry *e = &conn->inflight[i];
    if (!e->valid || e->acked || !e->reliable) continue;
    if (now_ms - e->send_time_ms <= timeout_ms) continue;

    /* Timeout - retransmit */
    if (!conn->cubic.in_recovery)
      cubic_on_loss(&conn->cubic, now_ms);
    e->retransmits++;
    conn->packets_lost++;
    conn->packets_retransmitted++;
    conn->fec_loss_counter++;

    uint32_t new_seq = conn->next_packet_seq++;
    uint8_t frame[MAX_DATAGRAM_SIZE];
    uint8_t retrans_prio = 0;
    struct reliable_stream *retrans_s = find_stream(conn, e->stream_id);
    if (retrans_s) retrans_prio = retrans_s->priority;
    build_header(frame, FRAME_TYPE_DATA, e->stream_id, retrans_prio);

    struct frame_data fd;
    fd.packet_seq = new_seq;
    fd.stream_seq = e->stream_seq;
    fd.data_len = e->payload_len;
    int body_off = frame_serialize_data_body(frame + FRAME_HEADER_SIZE, &fd);
    memcpy(frame + FRAME_HEADER_SIZE + body_off, e->payload, e->payload_len);

    uint16_t frame_len = FRAME_HEADER_SIZE + body_off + e->payload_len;

    /* Update inflight entry */
    e->packet_seq = new_seq;
    e->send_time_ms = now_ms;
    e->acked = 0;

    if (fb_add(&fb, frame, frame_len) != 0) {
      if (fb_send(&fb, conn) < 0) return -1;
      fb_init(&fb);
      if (fb_add(&fb, frame, frame_len) != 0) return -1;
    }
  }

  /* 2. Send pending data from streams (priority-ordered, round-robin) */
  for (int prio = 0; prio <= 3; prio++) {
    struct reliable_stream *s = conn->streams;
    while (s) {
      struct reliable_stream *next = s->next;
      if (s->state != STREAM_CLOSED && s->priority == prio) {
        if (send_stream_data(conn, s, &fb, now_ms) != 0)
          return -1;
      }
      s = next;
    }
  }

  /* 2b. Send pending STREAM_CLOSE frames for streams that have drained */
  {
    struct reliable_stream *s = conn->streams;
    while (s) {
      struct reliable_stream *next = s->next;
      if (s->state == STREAM_SEND_CLOSED && !s->sent_close_frame) {
        send_stream_close_frame(s);
        /* If no more data pending and remote closed, fully close */
        if (s->send_buf_head == s->send_buf_tail) {
          if (s->remote_closed) {
            s->state = STREAM_CLOSED;
            free_stream(conn, s);
          }
        }
      }
      s = next;
    }
  }

  /* 3. Send pending ACK (if no data was sent, send standalone ACK) */
  if (conn->pending_ack) {
    uint8_t ack_buf[MAX_DATAGRAM_SIZE];
    build_header(ack_buf, FRAME_TYPE_ACK, CONNECTION_STREAM_ID, 0);

    int ack_body_len = ack_tracker_generate(&conn->ack_tx, now_ms,
                                             ack_buf + FRAME_HEADER_SIZE,
                                             MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE);
    if (ack_body_len > 0) {
      uint16_t ack_total = FRAME_HEADER_SIZE + (uint16_t)ack_body_len;
      if (fb_add(&fb, ack_buf, ack_total) != 0) {
        if (fb_send(&fb, conn) < 0) return -1;
        fb_init(&fb);
        if (fb_add(&fb, ack_buf, ack_total) != 0) return -1;
      }
    }
    conn->pending_ack = 0;
    conn->last_ack_send_ms = now_ms;
  }

  /* 4. Timer-based ACK generation (every 10ms if no data flowing) */
  if (!conn->pending_ack && conn->last_ack_send_ms > 0 &&
      now_ms - conn->last_ack_send_ms >= 10) {
    uint8_t ack_buf[MAX_DATAGRAM_SIZE];
    build_header(ack_buf, FRAME_TYPE_ACK, CONNECTION_STREAM_ID, 0);

    int ack_body_len = ack_tracker_generate(&conn->ack_tx, now_ms,
                                             ack_buf + FRAME_HEADER_SIZE,
                                             MAX_DATAGRAM_SIZE - FRAME_HEADER_SIZE);
    if (ack_body_len > 0) {
      uint16_t ack_total = FRAME_HEADER_SIZE + (uint16_t)ack_body_len;
      if (fb_add(&fb, ack_buf, ack_total) != 0) {
        if (fb_send(&fb, conn) < 0) return -1;
        fb_init(&fb);
        if (fb_add(&fb, ack_buf, ack_total) < 0) return -1;
      }
      conn->last_ack_send_ms = now_ms;
    }
  }

  /* 5. Flush any remaining frames */
  return fb_send(&fb, conn);
}

int reliable_conn_input(struct reliable_conn *conn,
                         const void *pkt, uint32_t len,
                         uint32_t now_ms)
{
  if (!conn || !pkt || len == 0) return -1;

  const uint8_t *data = (const uint8_t *)pkt;
  uint32_t offset = 0;

  while (offset + FRAME_LENGTH_FIELD_SIZE <= len) {
    uint16_t frame_len = ((uint16_t)data[offset] << 8) | data[offset + 1];
    if (frame_len < FRAME_HEADER_SIZE) return -1;
    if (offset + FRAME_LENGTH_FIELD_SIZE + frame_len > len) return -1;

    const uint8_t *frame = data + offset + FRAME_LENGTH_FIELD_SIZE;
    offset += FRAME_LENGTH_FIELD_SIZE + frame_len;

    struct frame_header hdr;
    if (frame_deserialize_header(&hdr, frame) != FRAME_HEADER_SIZE)
      return -1;

    if (hdr.version != FRAME_VERSION) continue;

    conn->packets_received++;
    conn->bytes_received += frame_len;

    switch (hdr.type) {
      case FRAME_TYPE_DATA: {
        struct frame_data fd;
        const uint8_t *body = frame + FRAME_HEADER_SIZE;
        uint16_t body_len = frame_len - FRAME_HEADER_SIZE;
        if (body_len < FRAME_DATA_HEADER_SIZE) return -1;
        if (frame_deserialize_data_body(&fd, body) != FRAME_DATA_HEADER_SIZE)
          return -1;
        if (fd.data_len > body_len - FRAME_DATA_HEADER_SIZE) return -1;
        const uint8_t *payload = body + FRAME_DATA_HEADER_SIZE;
        process_data_frame(conn, hdr.stream_id, &fd, payload, now_ms);
        break;
      }

      case FRAME_TYPE_FEC: {
        const uint8_t *body = frame + FRAME_HEADER_SIZE;
        uint16_t body_len = frame_len - FRAME_HEADER_SIZE;
        if (body_len < FRAME_FEC_BODY_SIZE) return -1;
        struct frame_fec fec_frame;
        if (frame_deserialize_fec_body(&fec_frame, body) != FRAME_FEC_BODY_SIZE)
          return -1;
        const uint8_t *fec_payload = body + FRAME_FEC_BODY_SIZE;
        uint16_t fec_payload_len = body_len - FRAME_FEC_BODY_SIZE;
        if (fec_payload_len < fec_frame.data_len) return -1;
        process_fec_frame(conn, &fec_frame, fec_payload, now_ms);
        fec_try_decode(conn, now_ms);
        break;
      }

      case FRAME_TYPE_ACK: {
        const uint8_t *body = frame + FRAME_HEADER_SIZE;
        uint16_t body_len = frame_len - FRAME_HEADER_SIZE;
        struct frame_ack ack;
        struct ack_range ranges[MAX_ACK_RANGES];
        if (frame_deserialize_ack_body(&ack, ranges, MAX_ACK_RANGES,
                                         body, body_len) < 0)
          return -1;
        process_ack_frame(conn, &ack, ranges, now_ms);
        break;
      }

      case FRAME_TYPE_STREAM_OPEN: {
        const uint8_t *body = frame + FRAME_HEADER_SIZE;
        uint16_t body_len = frame_len - FRAME_HEADER_SIZE;
        if (body_len < 4) return -1;
        struct frame_stream_open open;
        if (frame_deserialize_stream_open_body(&open, body) != 4)
          return -1;
        /* Check if stream already exists */
        if (find_stream(conn, open.stream_id))
          break;
        struct reliable_stream *ns = reliable_stream_open(conn,
                                                           open.reliable,
                                                           open.ordered,
                                                           open.priority);
        if (ns)
          ns->stream_id = open.stream_id;
        break;
      }

      case FRAME_TYPE_STREAM_CLOSE: {
        struct reliable_stream *s = find_stream(conn, hdr.stream_id);
        if (s) {
          if (s->state == STREAM_SEND_CLOSED) {
            /* Both sides closed — fully close */
            s->state = STREAM_CLOSED;
            free_stream(conn, s);
          } else {
            /* Remote closed, we haven't closed yet */
            s->remote_closed = 1;
          }
        }
        break;
      }

      case FRAME_TYPE_PING: {
        /* Send PONG with same timestamp */
        uint64_t ts = 0;
        if (frame_len >= FRAME_HEADER_SIZE + 8) {
          const uint8_t *tb = frame + FRAME_HEADER_SIZE;
          ts = ((uint64_t)tb[0] << 56) | ((uint64_t)tb[1] << 48)
             | ((uint64_t)tb[2] << 40) | ((uint64_t)tb[3] << 32)
             | ((uint64_t)tb[4] << 24) | ((uint64_t)tb[5] << 16)
             | ((uint64_t)tb[6] << 8)  | tb[7];
        }
        uint8_t pong_buf[MAX_DATAGRAM_SIZE];
        int body_sz = frame_serialize_pong_body(
          pong_buf + FRAME_HEADER_SIZE, ts);
        if (body_sz > 0) {
          build_header(pong_buf, FRAME_TYPE_PONG, CONNECTION_STREAM_ID, 0);
          uint16_t pong_len = FRAME_HEADER_SIZE + (uint16_t)body_sz;
          struct frame_builder pong_fb;
          fb_init(&pong_fb);
          if (fb_add(&pong_fb, pong_buf, pong_len) == 0)
            conn->send_fn(conn->send_ctx, pong_fb.buf, pong_fb.offset,
                          (const struct sockaddr *)&conn->peer_addr,
                          sizeof(conn->peer_addr));
        }
        break;
      }

      case FRAME_TYPE_PONG: {
        /* RTT measurement from PING timestamp */
        if (frame_len >= FRAME_HEADER_SIZE + 8) {
          const uint8_t *tb = frame + FRAME_HEADER_SIZE;
          uint64_t ts = ((uint64_t)tb[0] << 56) | ((uint64_t)tb[1] << 48)
                       | ((uint64_t)tb[2] << 40) | ((uint64_t)tb[3] << 32)
                       | ((uint64_t)tb[4] << 24) | ((uint64_t)tb[5] << 16)
                       | ((uint64_t)tb[6] << 8)  | tb[7];
          /* Clamp to uint32_t for comparison with now_ms */
          uint32_t ts32 = (uint32_t)(ts & 0xFFFFFFFF);
          if (now_ms >= ts32) {
            uint32_t sample_ms = now_ms - ts32;
            rtt_update(&conn->rtt, sample_ms);
          }
        }
        break;
      }

      case FRAME_TYPE_WINDOW_UPDATE: {
        const uint8_t *body = frame + FRAME_HEADER_SIZE;
        uint16_t body_len = frame_len - FRAME_HEADER_SIZE;
        if (body_len < 4) return -1;
        struct frame_window_update wu;
        if (frame_deserialize_window_update_body(&wu, body) != 4)
          return -1;
        if (hdr.stream_id == CONNECTION_STREAM_ID) {
          conn->conn_send_window = wu.max_data;
        } else {
          struct reliable_stream *s = find_stream(conn, hdr.stream_id);
          if (s)
            s->flow.send_window = wu.max_data;
        }
        break;
      }

      default:
        break;
    }
  }

  return 0;
}

uint32_t reliable_conn_get_smoothed_rtt_ms(struct reliable_conn *conn)
{
  if (!conn) return INITIAL_RTT_MS;
  return Q16_TO_MS(conn->rtt.smoothed_rtt);
}

uint32_t reliable_conn_get_min_rtt_ms(struct reliable_conn *conn)
{
  if (!conn) return INITIAL_RTT_MS;
  return conn->rtt.min_rtt;
}

uint32_t reliable_time_ms(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t)((int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
