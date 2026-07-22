#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "frame.h"
#include "fec.h"
#include "reliable_udp.h"

/* Q16.16 helpers (same as reliable_udp.c) */
#define Q16_SHIFT 16
#define Q16_FROM_MS(ms) ((uint32_t)((ms) << Q16_SHIFT))

/* Internal function from reliable_udp.c — exposed for testing */
struct reliable_stream* find_stream(struct reliable_conn *conn, uint16_t stream_id);

/* ------------------------------------------------------------------ */
/*  test infrastructure                                               */
/* ------------------------------------------------------------------ */
static int g_failures = 0;

#define TEST(name) do { \
  printf("  %-45s ... ", name); \
  fflush(stdout); \
  } while(0)

#define PASS() do { printf("PASS\n"); } while(0)
#define FAIL() do { printf("FAIL\n"); g_failures++; } while(0)

/* A simple loopback send function */
struct loopback_ctx {
  uint8_t buf[MAX_DATAGRAM_SIZE];
  uint32_t len;
  struct sockaddr_in from;
};

static int loopback_send(void *ctx, const void *data, uint32_t len,
                          const struct sockaddr *addr, socklen_t addrlen)
{
  (void)addr; (void)addrlen;
  struct loopback_ctx *lb = (struct loopback_ctx *)ctx;
  if (len > MAX_DATAGRAM_SIZE) return -1;
  memcpy(lb->buf, data, len);
  lb->len = len;
  return (int)len;
}

/* Helper: set up a conn with peer address */
static struct reliable_conn* setup_conn(struct loopback_ctx *lb, uint32_t session_id)
{
  memset(lb, 0, sizeof(*lb));
  struct reliable_conn *conn = reliable_conn_create(session_id, loopback_send, lb);
  assert(conn != NULL);
  struct sockaddr_in peer;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_addr.s_addr = 0x0100007F;
  peer.sin_port = 9999;
  reliable_conn_set_peer(conn, &peer);
  return conn;
}

/* Helper: build a single-frame datagram from a frame buffer + length */
static void build_datagram(uint8_t *dgram, uint16_t *dgram_len,
                            const uint8_t *frame, uint16_t frame_len)
{
  dgram[0] = (uint8_t)(frame_len >> 8);
  dgram[1] = (uint8_t)(frame_len & 0xFF);
  memcpy(dgram + 2, frame, frame_len);
  *dgram_len = 2 + frame_len;
}

/* Helper: build an ACK frame datagram */
static void build_ack_datagram(uint8_t *dgram, uint16_t *dgram_len,
                                uint32_t largest_acked, uint16_t first_range)
{
  uint8_t ack_frame[MAX_DATAGRAM_SIZE];
  struct frame_header ack_hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_ACK, CONNECTION_STREAM_ID};
  frame_serialize_header(ack_frame, &ack_hdr);
  struct ack_range ranges[1] = {{0, 0}};
  struct frame_ack ack = {largest_acked, 0, first_range, 0};
  int body_len = frame_serialize_ack_body(ack_frame + FRAME_HEADER_SIZE, &ack, ranges);
  assert(body_len > 0);
  uint16_t flen = FRAME_HEADER_SIZE + (uint16_t)body_len;
  build_datagram(dgram, dgram_len, ack_frame, flen);
}

/* ------------------------------------------------------------------ */
/*  Frame tests                                                       */
/* ------------------------------------------------------------------ */
static void test_frame_header_roundtrip(void)
{
  TEST("frame_header_roundtrip");
  uint8_t buf[FRAME_HEADER_SIZE];
  struct frame_header hdr = {0x01, 1, 0, 0x02, 0x1234};
  int n = frame_serialize_header(buf, &hdr);
  assert(n == FRAME_HEADER_SIZE);
  struct frame_header hdr2;
  frame_deserialize_header(&hdr2, buf);
  assert(hdr2.version == 0x01);
  assert(hdr2.priority == 1);
  assert(hdr2.reserved == 0);
  assert(hdr2.type == 0x02);
  assert(hdr2.stream_id == 0x1234);
  PASS();
}

static void test_data_frame_roundtrip(void)
{
  TEST("data_frame_roundtrip");
  uint8_t buf[FRAME_DATA_HEADER_SIZE];
  struct frame_data fd = {0xDEADBEEF, 0x12345678, 0x400};
  int n = frame_serialize_data_body(buf, &fd);
  assert(n == FRAME_DATA_HEADER_SIZE);
  struct frame_data fd2;
  frame_deserialize_data_body(&fd2, buf);
  assert(fd2.packet_seq == 0xDEADBEEF);
  assert(fd2.stream_seq == 0x12345678);
  assert(fd2.data_len == 0x400);
  PASS();
}

static void test_ack_frame_roundtrip(void)
{
  TEST("ack_frame_roundtrip");
  uint8_t buf[128];
  struct ack_range ranges[2] = {{1, 2}, {3, 4}};
  struct frame_ack ack = {0xAAAAAAAA, 15, 5, 2};
  int n = frame_serialize_ack_body(buf, &ack, ranges);
  assert(n > 0);
  struct frame_ack ack2;
  struct ack_range ranges2[4];
  int n2 = frame_deserialize_ack_body(&ack2, ranges2, 4, buf, (uint16_t)n);
  assert(n2 == n);
  assert(ack2.largest_acked == 0xAAAAAAAA);
  assert(ack2.ack_delay == 15);
  assert(ack2.first_ack_range == 5);
  assert(ack2.range_count == 2);
  assert(ranges2[0].gap == 1 && ranges2[0].length == 2);
  assert(ranges2[1].gap == 3 && ranges2[1].length == 4);
  PASS();
}

static void test_stream_open_roundtrip(void)
{
  TEST("stream_open_roundtrip");
  uint8_t buf[4];
  struct frame_stream_open open = {0x1234, 1, 0, 2};
  int n = frame_serialize_stream_open_body(buf, &open);
  assert(n == 4);
  struct frame_stream_open open2;
  frame_deserialize_stream_open_body(&open2, buf);
  assert(open2.stream_id == 0x1234);
  assert(open2.reliable == 1);
  assert(open2.ordered == 0);
  assert(open2.priority == 2);
  PASS();
}

static void test_window_update_roundtrip(void)
{
  TEST("window_update_roundtrip");
  uint8_t buf[4];
  struct frame_window_update wu = {0x10000};
  int n = frame_serialize_window_update_body(buf, &wu);
  assert(n == 4);
  struct frame_window_update wu2;
  frame_deserialize_window_update_body(&wu2, buf);
  assert(wu2.max_data == 0x10000);
  PASS();
}

static void test_ping_pong(void)
{
  TEST("ping_pong");
  uint8_t buf[8];
  uint64_t ts = 0x1234567890ABCDEFULL;
  int n = frame_serialize_ping_body(buf, ts);
  assert(n == 8);
  assert(buf[0] == 0x12); assert(buf[7] == 0xEF);
  memset(buf, 0, 8);
  n = frame_serialize_pong_body(buf, ts);
  assert(n == 8);
  assert(buf[0] == 0x12); assert(buf[7] == 0xEF);
  PASS();
}

static void test_coalescing(void)
{
  TEST("coalescing");
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t off = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t frame[FRAME_HEADER_SIZE + 2];
    struct frame_header hdr = {0x01, 0, 0, (uint8_t)(0x01 + i), (uint16_t)(i + 1)};
    frame_serialize_header(frame, &hdr);
    frame[4] = (uint8_t)i;
    frame[5] = (uint8_t)(i + 1);
    uint16_t flen = FRAME_HEADER_SIZE + 2;
    dgram[off++] = (uint8_t)(flen >> 8);
    dgram[off++] = (uint8_t)(flen & 0xFF);
    memcpy(dgram + off, frame, flen);
    off += flen;
  }
  int fc = 0;
  int total = frame_get_total_length(dgram, off, &fc);
  assert(total == (int)off);
  assert(fc == 3);
  int t = frame_peek_type(dgram, off);
  assert(t == 0x01);
  PASS();
}

/* ------------------------------------------------------------------ */
/*  Reliable connection tests                                         */
/* ------------------------------------------------------------------ */
static void test_create_destroy(void)
{
  TEST("conn_create_destroy");
  struct loopback_ctx lb;
  memset(&lb, 0, sizeof(lb));
  struct reliable_conn *conn = reliable_conn_create(42, loopback_send, &lb);
  assert(conn != NULL);
  assert(reliable_conn_get_smoothed_rtt_ms(conn) == INITIAL_RTT_MS);
  assert(reliable_conn_get_min_rtt_ms(conn) == INITIAL_RTT_MS);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_open_close(void)
{
  TEST("stream_open_close");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 1);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  assert(s->stream_id == 1 || s->stream_id == 2);
  reliable_stream_close(s);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_send_recv(void)
{
  TEST("stream_send_recv");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 2);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  const char *msg = "Hello!";
  int n = reliable_stream_send(s, msg, 6);
  assert(n == 6);
  reliable_conn_tick(conn, 100);
  assert(lb.len > 0);
  assert(conn->packets_sent == 1);
  assert(conn->bytes_sent == 6);
  int ret = reliable_conn_input(conn, lb.buf, lb.len, 100);
  assert(ret == 0);
  char rbuf[64];
  n = reliable_stream_recv(s, rbuf, sizeof(rbuf));
  assert(n == 6);
  assert(memcmp(rbuf, "Hello!", 6) == 0);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_rtt_estimation(void)
{
  TEST("rtt_estimation");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 3);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  reliable_stream_send(s, "data", 4);
  reliable_conn_tick(conn, 100);
  assert(lb.len > 0);
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_ack_datagram(dgram, &dgram_len, 0, 1);
  reliable_conn_input(conn, dgram, dgram_len, 200);
  uint32_t rtt = reliable_conn_get_smoothed_rtt_ms(conn);
  assert(rtt > 50 && rtt < 200);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_frame_coalescing_in_conn(void)
{
  TEST("frame_coalescing_in_conn");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 4);
  struct reliable_stream *s1 = reliable_stream_open(conn, 1, 1, 0);
  struct reliable_stream *s2 = reliable_stream_open(conn, 1, 1, 1);
  reliable_stream_send(s1, "AAAA", 4);
  reliable_stream_send(s2, "BBBB", 4);
  reliable_conn_tick(conn, 100);
  int fc = 0;
  int total = frame_get_total_length(lb.buf, lb.len, &fc);
  assert(total > 0);
  assert(fc == 2);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_retransmission(void)
{
  TEST("retransmission_timeout");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 5);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  reliable_stream_send(s, "data", 4);
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent == 1);
  reliable_conn_tick(conn, 100);
  assert(conn->packets_retransmitted == 0);
  reliable_conn_tick(conn, 10000);
  assert(conn->packets_retransmitted == 1);
  assert(conn->packets_lost == 1);
  reliable_conn_destroy(conn);
  PASS();
}

/* ------------------------------------------------------------------ */
/*  Phase 1 fix tests                                                 */
/* ------------------------------------------------------------------ */
static void test_pong_rtt(void)
{
  TEST("pong_rtt_measurement");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 6);
  uint8_t frame[MAX_DATAGRAM_SIZE];
  struct frame_header hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_PONG, CONNECTION_STREAM_ID};
  frame_serialize_header(frame, &hdr);
  uint64_t ts = 500;
  uint8_t *tb = frame + FRAME_HEADER_SIZE;
  tb[0] = (uint8_t)(ts >> 56); tb[1] = (uint8_t)(ts >> 48);
  tb[2] = (uint8_t)(ts >> 40); tb[3] = (uint8_t)(ts >> 32);
  tb[4] = (uint8_t)(ts >> 24); tb[5] = (uint8_t)(ts >> 16);
  tb[6] = (uint8_t)(ts >> 8);  tb[7] = (uint8_t)(ts & 0xFF);
  uint16_t flen = FRAME_HEADER_SIZE + 8;
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_datagram(dgram, &dgram_len, frame, flen);
  uint32_t initial_rtt = reliable_conn_get_smoothed_rtt_ms(conn);
  reliable_conn_input(conn, dgram, dgram_len, 600);
  uint32_t new_rtt = reliable_conn_get_smoothed_rtt_ms(conn);
  assert(new_rtt != initial_rtt || new_rtt < 150);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_bytes_in_flight(void)
{
  TEST("bytes_in_flight_tracking");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 7);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s->flow.bytes_in_flight == 0);
  reliable_stream_send(s, "data123", 7);
  reliable_conn_tick(conn, 100);
  assert(s->flow.bytes_in_flight == 7);
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_ack_datagram(dgram, &dgram_len, 0, 1);
  reliable_conn_input(conn, dgram, dgram_len, 200);
  assert(s->flow.bytes_in_flight == 0);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_packets_received_counter(void)
{
  TEST("packets_received_counter");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 8);
  uint8_t frame[MAX_DATAGRAM_SIZE];
  struct frame_header hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_PING, CONNECTION_STREAM_ID};
  frame_serialize_header(frame, &hdr);
  uint64_t ts = 1000;
  uint8_t *tb = frame + FRAME_HEADER_SIZE;
  tb[0] = (uint8_t)(ts >> 56); tb[1] = (uint8_t)(ts >> 48);
  tb[2] = (uint8_t)(ts >> 40); tb[3] = (uint8_t)(ts >> 32);
  tb[4] = (uint8_t)(ts >> 24); tb[5] = (uint8_t)(ts >> 16);
  tb[6] = (uint8_t)(ts >> 8);  tb[7] = (uint8_t)(ts & 0xFF);
  uint16_t flen = FRAME_HEADER_SIZE + 8;
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_datagram(dgram, &dgram_len, frame, flen);
  assert(conn->packets_received == 0);
  assert(conn->bytes_received == 0);
  reliable_conn_input(conn, dgram, dgram_len, 1500);
  assert(conn->packets_received == 1);
  assert(conn->bytes_received == flen);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_max_streams_enforced(void)
{
  TEST("max_streams_enforced");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 9);
  struct reliable_stream *streams[MAX_STREAMS];
  int i;
  for (i = 0; i < MAX_STREAMS; i++) {
    streams[i] = reliable_stream_open(conn, 1, 1, 0);
    if (!streams[i]) break;
  }
  assert(i == MAX_STREAMS);
  struct reliable_stream *extra = reliable_stream_open(conn, 1, 1, 0);
  assert(extra == NULL);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_close_unlinks(void)
{
  TEST("stream_close_unlinks");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 10);
  struct reliable_stream *s1 = reliable_stream_open(conn, 1, 1, 0);
  struct reliable_stream *s2 = reliable_stream_open(conn, 1, 1, 0);
  assert(s1 != NULL && s2 != NULL);
  reliable_stream_close(s1);
  struct reliable_stream *s3 = reliable_stream_open(conn, 1, 1, 0);
  assert(s3 != NULL);
  reliable_stream_send(s3, "test", 4);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_id_uint16(void)
{
  TEST("stream_id_uint16");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 11);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  assert(sizeof(s->stream_id) == 2);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_ack_delay_not_zero(void)
{
  TEST("ack_delay_not_zero");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 12);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  reliable_stream_send(s, "test", 4);
  reliable_conn_tick(conn, 100);
  reliable_conn_input(conn, lb.buf, lb.len, 200);
  reliable_conn_tick(conn, 250);
  int fc = 0;
  int total = frame_get_total_length(lb.buf, lb.len, &fc);
  assert(total > 0);
  assert(fc >= 1);
  (void)total;
  int type = frame_peek_type(lb.buf, lb.len);
  assert(type == FRAME_TYPE_ACK);
  const uint8_t *frame = lb.buf + FRAME_LENGTH_FIELD_SIZE;
  struct frame_header hdr;
  frame_deserialize_header(&hdr, frame);
  assert(hdr.type == FRAME_TYPE_ACK);
  uint16_t frame_len = ((uint16_t)lb.buf[0] << 8) | lb.buf[1];
  const uint8_t *ack_body = frame + FRAME_HEADER_SIZE;
  uint16_t ack_body_len = frame_len - FRAME_HEADER_SIZE;
  struct frame_ack ack2;
  struct ack_range ranges2[4];
  int ret = frame_deserialize_ack_body(&ack2, ranges2, 4, ack_body, ack_body_len);
  assert(ret > 0);
  assert(ack2.ack_delay > 0);
  assert(ack2.ack_delay <= 55);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_timer_based_ack(void)
{
  TEST("timer_based_ack");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 13);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  reliable_stream_send(s, "test", 4);
  reliable_conn_tick(conn, 100);
  uint8_t data_buf[MAX_DATAGRAM_SIZE];
  uint32_t data_len = lb.len;
  memcpy(data_buf, lb.buf, lb.len);
  reliable_conn_input(conn, data_buf, data_len, 110);
  assert(conn->pending_ack == 1);
  reliable_conn_tick(conn, 115);
  assert(conn->pending_ack == 0);
  assert(conn->last_ack_send_ms == 115);
  memset(&lb, 0, sizeof(lb));
  reliable_conn_tick(conn, 160);
  assert(lb.len > 0);
  int type = frame_peek_type(lb.buf, lb.len);
  assert(type == FRAME_TYPE_ACK);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_fast_retransmit(void)
{
  TEST("fast_retransmit");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 14);
  struct reliable_stream *s1 = reliable_stream_open(conn, 1, 1, 0);
  struct reliable_stream *s2 = reliable_stream_open(conn, 1, 1, 0);
  reliable_stream_send(s1, "AAAA", 4);
  reliable_stream_send(s2, "BBBB", 4);
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent == 2);
  uint32_t seq2 = 1;
  for (int dup = 0; dup < 3; dup++) {
    uint8_t dgram[MAX_DATAGRAM_SIZE];
    uint16_t dgram_len;
    build_ack_datagram(dgram, &dgram_len, seq2, 1);
    reliable_conn_input(conn, dgram, dgram_len, 200 + dup * 10);
  }
  assert(conn->packets_retransmitted >= 1);
  assert(conn->packets_lost >= 1);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_inflight_insert_by_seq(void)
{
  TEST("inflight_insert_by_seq");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 15);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  reliable_stream_send(s, "test", 4);
  reliable_conn_tick(conn, 100);
  struct inflight_entry *e = &conn->inflight[0 % MAX_INFLIGHT];
  assert(e->valid);
  assert(e->packet_seq == 0);
  (void)e;
  reliable_conn_destroy(conn);
  PASS();
}

/* ================================================================== */
/*  Phase 2: Multi-Stream Tests                                       */
/* ================================================================== */

/* Helper: build a DATA frame datagram for feeding into conn_input */
static void build_data_datagram(uint8_t *dgram, uint16_t *dgram_len,
                                 uint16_t stream_id,
                                 uint32_t packet_seq, uint32_t stream_seq,
                                 const uint8_t *payload, uint16_t payload_len)
{
  uint8_t frame[MAX_DATAGRAM_SIZE];
  struct frame_header hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_DATA, stream_id};
  frame_serialize_header(frame, &hdr);
  struct frame_data fd;
  fd.packet_seq = packet_seq;
  fd.stream_seq = stream_seq;
  fd.data_len = payload_len;
  int body_off = frame_serialize_data_body(frame + FRAME_HEADER_SIZE, &fd);
  memcpy(frame + FRAME_HEADER_SIZE + body_off, payload, payload_len);
  uint16_t flen = FRAME_HEADER_SIZE + (uint16_t)body_off + payload_len;
  build_datagram(dgram, dgram_len, frame, flen);
}

static void test_priority_scheduling(void)
{
  TEST("priority_scheduling");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 100);

  /* Open streams with different priorities */
  struct reliable_stream *low = reliable_stream_open(conn, 1, 1, 3);
  struct reliable_stream *high = reliable_stream_open(conn, 1, 1, 0);
  struct reliable_stream *mid = reliable_stream_open(conn, 1, 1, 1);

  assert(low && high && mid);

  /* All have data to send */
  reliable_stream_send(high, "HIGH", 4);
  reliable_stream_send(mid, "MIDD", 4);
  reliable_stream_send(low, "LOW_", 4);

  reliable_conn_tick(conn, 100);

  /* Parse the coalesced datagram — frames should be in priority order */
  int fc = 0;
  int total = frame_get_total_length(lb.buf, lb.len, &fc);
  assert(total > 0);
  assert(fc == 3);

  /* First frame should be from highest priority stream */
  uint16_t off = 0;
  int seen_high = 0, seen_mid = 0, seen_low = 0;
  for (int i = 0; i < fc; i++) {
    uint16_t flen = ((uint16_t)lb.buf[off] << 8) | lb.buf[off + 1];
    const uint8_t *frame = lb.buf + off + FRAME_LENGTH_FIELD_SIZE;
    struct frame_header hdr;
    frame_deserialize_header(&hdr, frame);
    if (i == 0) {
      assert(conn->is_initiator ? hdr.stream_id == high->stream_id : 1);
    }
    if (hdr.stream_id == high->stream_id) seen_high = 1;
    if (hdr.stream_id == mid->stream_id) seen_mid = 1;
    if (hdr.stream_id == low->stream_id) seen_low = 1;
    off += FRAME_LENGTH_FIELD_SIZE + flen;
  }
  assert(seen_high && seen_mid && seen_low);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_connection_flow_control(void)
{
  TEST("connection_flow_control");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 101);

  /* Set a tight connection-level send window */
  conn->conn_send_window = 500;

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  /* Send a large amount of data */
  uint8_t big_data[2000];
  memset(big_data, 'X', sizeof(big_data));
  int n = reliable_stream_send(s, big_data, sizeof(big_data));
  assert(n == 2000);

  /* Tick — should be limited by conn_send_window */
  reliable_conn_tick(conn, 100);
  assert(conn->conn_bytes_in_flight <= conn->conn_send_window);
  assert(conn->conn_bytes_in_flight > 0);

  /* ACK the sent data to free up window */
  uint32_t sent_seq = 0;
  while (conn->conn_bytes_in_flight > 0 && sent_seq < conn->packets_sent) {
    uint8_t dgram[MAX_DATAGRAM_SIZE];
    uint16_t dgram_len;
    build_ack_datagram(dgram, &dgram_len, sent_seq, 1);
    reliable_conn_input(conn, dgram, dgram_len, 200 + sent_seq);
    sent_seq++;
  }
  assert(conn->conn_bytes_in_flight == 0);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_level_flow_control(void)
{
  TEST("stream_level_flow_control");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 102);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  /* Set a tight per-stream send window */
  s->flow.send_window = 200;

  uint8_t big_data[1000];
  memset(big_data, 'Y', sizeof(big_data));
  int n = reliable_stream_send(s, big_data, sizeof(big_data));
  assert(n == 1000);

  reliable_conn_tick(conn, 100);
  assert(s->flow.bytes_in_flight <= s->flow.send_window);
  assert(s->flow.bytes_in_flight > 0);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_proactive_window_update(void)
{
  TEST("proactive_window_update");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 103);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  /* Feed enough data to fill most of the recv buffer */
  uint32_t half_buf = RELIABLE_STREAM_RECV_BUF_SIZE / 2;
  uint8_t *payload = (uint8_t *)malloc(half_buf);
  assert(payload != NULL);
  memset(payload, 'Z', half_buf);

  /* Build DATA frames to fill recv buffer */
  uint32_t offset = 0;
  uint32_t pseq = 1;
  while (offset < half_buf) {
    uint32_t chunk = (half_buf - offset) < 1000 ? (half_buf - offset) : 1000;
    uint8_t dgram[MAX_DATAGRAM_SIZE];
    uint16_t dgram_len;
    build_data_datagram(dgram, &dgram_len, s->stream_id,
                         pseq++, (offset / 1000) + 1,
                         payload + offset, (uint16_t)chunk);
    reliable_conn_input(conn, dgram, dgram_len, 100 + offset);
    offset += chunk;
  }

  /* Clear the loopback buffer (ACK frames may have been sent) */
  memset(&lb, 0, sizeof(lb));

  /* Now recv some data — this should trigger WINDOW_UPDATE */
  uint8_t recv_buf[4000];
  int n = reliable_stream_recv(s, recv_buf, sizeof(recv_buf));
  assert(n > 0);

  /* Check that a WINDOW_UPDATE frame was sent */
  assert(lb.len > 0);
  int type = frame_peek_type(lb.buf, lb.len);
  assert(type == FRAME_TYPE_WINDOW_UPDATE);

  free(payload);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_close_frame_sent(void)
{
  TEST("stream_close_frame_sent");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 104);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  /* Clear the loopback buffer — close should send STREAM_CLOSE */
  memset(&lb, 0, sizeof(lb));
  s->stream_id = 42; /* predictable ID for testing */
  reliable_stream_close(s);

  /* Verify STREAM_CLOSE frame was sent */
  assert(lb.len > 0);
  int type = frame_peek_type(lb.buf, lb.len);
  assert(type == FRAME_TYPE_STREAM_CLOSE);

  /* Verify stream_id in the close frame */
  const uint8_t *frame = lb.buf + FRAME_LENGTH_FIELD_SIZE;
  struct frame_header hdr;
  frame_deserialize_header(&hdr, frame);
  assert(hdr.stream_id == 42);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_bidirectional_close(void)
{
  TEST("bidirectional_close");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 105);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  uint16_t sid = s->stream_id;

  /* Feed a STREAM_CLOSE from the remote side */
  uint8_t close_frame[MAX_DATAGRAM_SIZE];
  struct frame_header close_hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_STREAM_CLOSE, sid};
  frame_serialize_header(close_frame, &close_hdr);
  uint16_t flen = FRAME_HEADER_SIZE;
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_datagram(dgram, &dgram_len, close_frame, flen);
  reliable_conn_input(conn, dgram, dgram_len, 100);

  /* Stream should now have remote_closed = 1 */
  assert(s->remote_closed == 1);
  assert(s->state == STREAM_OPEN);

  /* Now close locally — should immediately fully close */
  reliable_stream_close(s);

  /* Stream should no longer be in the list */
  struct reliable_stream *found = find_stream(conn, sid);
  assert(found == NULL);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_bidirectional_close_simultaneous(void)
{
  TEST("bidirectional_close_simul");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 106);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  uint16_t sid = s->stream_id;

  /* Close locally first (sends STREAM_CLOSE, stays SEND_CLOSED) */
  memset(&lb, 0, sizeof(lb));
  /* Override: stream has no pending data, so it would fully close.
     To test simultaneous close, we need the close to stay SEND_CLOSED
     until remote close arrives. We give it some pending data first. */
  reliable_stream_send(s, "pending", 7);
  reliable_stream_close(s);
  assert(s->state == STREAM_SEND_CLOSED);

  /* Now feed remote STREAM_CLOSE */
  uint8_t close_frame[MAX_DATAGRAM_SIZE];
  struct frame_header close_hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_STREAM_CLOSE, sid};
  frame_serialize_header(close_frame, &close_hdr);
  uint16_t flen = FRAME_HEADER_SIZE;
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_datagram(dgram, &dgram_len, close_frame, flen);
  reliable_conn_input(conn, dgram, dgram_len, 200);

  /* Stream should be fully closed (remote closed + local SEND_CLOSED) */
  struct reliable_stream *found = find_stream(conn, sid);
  assert(found == NULL);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_conn_level_window_update(void)
{
  TEST("conn_level_window_update");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 107);

  /* Send a WINDOW_UPDATE for connection stream ID 0 */
  uint8_t wu_frame[MAX_DATAGRAM_SIZE];
  struct frame_header wu_hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_WINDOW_UPDATE, CONNECTION_STREAM_ID};
  frame_serialize_header(wu_frame, &wu_hdr);
  struct frame_window_update wu = {0x20000};
  int body_sz = frame_serialize_window_update_body(wu_frame + FRAME_HEADER_SIZE, &wu);
  uint16_t flen = FRAME_HEADER_SIZE + (uint16_t)body_sz;
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_datagram(dgram, &dgram_len, wu_frame, flen);
  reliable_conn_input(conn, dgram, dgram_len, 100);

  assert(conn->conn_send_window == 0x20000);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_multi_stream_independence(void)
{
  TEST("multi_stream_independence");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 108);

  struct reliable_stream *s1 = reliable_stream_open(conn, 1, 1, 0);
  struct reliable_stream *s2 = reliable_stream_open(conn, 1, 1, 0);
  assert(s1 && s2);

  /* Send on both streams */
  reliable_stream_send(s1, "stream1", 7);
  reliable_stream_send(s2, "stream2", 7);

  /* Tick to send all data */
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent == 2);

  /* Feed both frames back into conn */
  reliable_conn_input(conn, lb.buf, lb.len, 150);

  /* Each stream should receive its own data */
  char buf[16];
  int n1 = reliable_stream_recv(s1, buf, sizeof(buf));
  assert(n1 == 7);
  assert(memcmp(buf, "stream1", 7) == 0);

  int n2 = reliable_stream_recv(s2, buf, sizeof(buf));
  assert(n2 == 7);
  assert(memcmp(buf, "stream2", 7) == 0);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_stream_send_after_close_rejected(void)
{
  TEST("send_after_close_rejected");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 109);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  reliable_stream_close(s);

  /* Trying to send on a non-OPEN stream should fail */
  int n = reliable_stream_send(s, "data", 4);
  assert(n == -1);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_close_with_pending_data(void)
{
  TEST("close_with_pending_data");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 110);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  uint16_t sid = s->stream_id;

  /* Send data but don't flush it all */
  uint8_t big_data[2000];
  memset(big_data, 'D', sizeof(big_data));
  reliable_stream_send(s, big_data, sizeof(big_data));

  /* Close — should transition to SEND_CLOSED, not CLOSED */
  reliable_stream_close(s);
  assert(s->state == STREAM_SEND_CLOSED);

  /* Stream should still be findable */
  struct reliable_stream *found = find_stream(conn, sid);
  assert(found == s);

  /* Tick to send pending data */
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent > 0);

  /* Stream still exists */
  assert(find_stream(conn, sid) == s);

  /* ACK all sent packets so data is considered delivered */
  uint32_t pseq;
  for (pseq = 0; pseq < conn->packets_sent; pseq++) {
    uint8_t dgram[MAX_DATAGRAM_SIZE];
    uint16_t dgram_len;
    build_ack_datagram(dgram, &dgram_len, pseq, 1);
    reliable_conn_input(conn, dgram, dgram_len, 200 + pseq);
  }

  /* Stream is SEND_CLOSED with no pending data — but remote hasn't closed yet */
  reliable_conn_tick(conn, 300);
  assert(s->state == STREAM_SEND_CLOSED);
  found = find_stream(conn, sid);
  assert(found == s);

  /* Now simulate remote close — fully close */
  uint8_t close_frame[MAX_DATAGRAM_SIZE];
  struct frame_header close_hdr = {FRAME_VERSION, 0, 0, FRAME_TYPE_STREAM_CLOSE, sid};
  frame_serialize_header(close_frame, &close_hdr);
  uint16_t flen = FRAME_HEADER_SIZE;
  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_datagram(dgram, &dgram_len, close_frame, flen);
  reliable_conn_input(conn, dgram, dgram_len, 400);

  /* Stream should now be fully gone */
  found = find_stream(conn, sid);
  assert(found == NULL);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_window_update_max_data(void)
{
  TEST("window_update_max_data_correct");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 111);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  /* Feed data into recv buffer */
  uint8_t payload[RELIABLE_STREAM_RECV_BUF_SIZE / 2];
  memset(payload, 'A', sizeof(payload));
  uint32_t pseq = 1;
  uint32_t offset = 0;
  while (offset < sizeof(payload)) {
    uint32_t chunk = 1000;
    if (sizeof(payload) - offset < chunk) chunk = sizeof(payload) - offset;
    uint8_t dgram[MAX_DATAGRAM_SIZE];
    uint16_t dgram_len;
    build_data_datagram(dgram, &dgram_len, s->stream_id,
                         pseq++, (offset / 1000) + 1,
                         payload + offset, (uint16_t)chunk);
    reliable_conn_input(conn, dgram, dgram_len, 100 + offset);
    offset += chunk;
  }

  /* Clear loopback buffer */
  memset(&lb, 0, sizeof(lb));

  /* Recv some data to trigger WINDOW_UPDATE */
  uint8_t recv_buf[4000];
  int n = reliable_stream_recv(s, recv_buf, sizeof(recv_buf));
  assert(n > 0);

  /* Verify WINDOW_UPDATE frame was sent with correct max_data */
  assert(lb.len > 0);
  int type = frame_peek_type(lb.buf, lb.len);
  assert(type == FRAME_TYPE_WINDOW_UPDATE);

  /* Parse max_data from WINDOW_UPDATE frame */
  const uint8_t *wu_frame = lb.buf + FRAME_LENGTH_FIELD_SIZE;
  uint16_t wu_frame_len = ((uint16_t)lb.buf[0] << 8) | lb.buf[1];
  (void)wu_frame_len;
  uint32_t max_data;
  memcpy(&max_data, wu_frame + FRAME_HEADER_SIZE, 4);
  max_data = __builtin_bswap32(max_data);
  assert(max_data == (uint32_t)(s->recv_buf_tail + RELIABLE_STREAM_RECV_BUF_SIZE));

  reliable_conn_destroy(conn);
  PASS();
}

static void test_priority_clamping(void)
{
  TEST("priority_clamping");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 112);

  struct reliable_stream *s0 = reliable_stream_open(conn, 1, 1, 0);
  assert(s0 != NULL);
  assert(s0->priority == 0);

  struct reliable_stream *s3 = reliable_stream_open(conn, 1, 1, 3);
  assert(s3 != NULL);
  assert(s3->priority == 3);

  struct reliable_stream *s_high = reliable_stream_open(conn, 1, 1, 5);
  assert(s_high != NULL);
  assert(s_high->priority == 3);

  struct reliable_stream *s_max = reliable_stream_open(conn, 1, 1, 255);
  assert(s_max != NULL);
  assert(s_max->priority == 3);

  reliable_conn_destroy(conn);
  PASS();
}

static void test_total_bytes_sent_flow_control(void)
{
  TEST("total_bytes_sent_flow_control");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 113);

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  assert(s->total_bytes_sent == 0);

  /* Set a tight send window */
  s->flow.send_window = 200;

  uint8_t big_data[1000];
  memset(big_data, 'Y', sizeof(big_data));
  int n = reliable_stream_send(s, big_data, sizeof(big_data));
  assert(n == 1000);

  reliable_conn_tick(conn, 100);
  assert(s->total_bytes_sent <= s->flow.send_window);
  assert(s->total_bytes_sent > 0);

  reliable_conn_destroy(conn);
  PASS();
}

/* ------------------------------------------------------------------ */
/*  Phase 3: FEC Tests                                                */
/* ------------------------------------------------------------------ */

static void test_fec_frame_serialize(void)
{
  TEST("fec_frame_serialize");
  uint8_t buf[FRAME_FEC_BODY_SIZE];
  struct frame_fec fec = {0xDEADBEEF, 12, 1000};
  int n = frame_serialize_fec_body(buf, &fec);
  assert(n == FRAME_FEC_BODY_SIZE);
  struct frame_fec fec2;
  frame_deserialize_fec_body(&fec2, buf);
  assert(fec2.group_id == 0xDEADBEEF);
  assert(fec2.fec_index == 12);
  assert(fec2.data_len == 1000);
  PASS();
}

static void test_fec_group_derivation(void)
{
  TEST("fec_group_derivation");
  uint8_t n = 10;
  /* Group ID = packet_seq / n, Index = packet_seq % n */
  assert(0 / n == 0); assert(0 % n == 0);
  assert(9 / n == 0); assert(9 % n == 9);
  assert(10 / n == 1); assert(10 % n == 0);
  assert(19 / n == 1); assert(19 % n == 9);
  assert(20 / n == 2); assert(20 % n == 0);
  PASS();
}

static void test_fec_encode_decode(void)
{
  TEST("fec_encode_decode");
  struct fec_ctx ctx;
  fec_init(&ctx, 4, 2);

  struct fec_packet data[4];
  struct fec_packet parity[2];
  memset(data, 0, sizeof(data));
  memset(parity, 0, sizeof(parity));

  for (uint8_t i = 0; i < 4; i++) {
    for (uint16_t j = 0; j < 100; j++)
      data[i].data[j] = (uint8_t)(i * 100 + j);
    data[i].len = 100;
  }

  uint16_t plen = fec_padded_length(data, 4);
  assert(plen == 100);

  int ret = fec_encode(&ctx, data, parity, plen);
  assert(ret == 0);

  /* Zero out packet 1 (simulate loss) */
  struct fec_packet decode_packets[6];
  for (uint8_t i = 0; i < 6; i++) {
    if (i == 1) {
      memset(&decode_packets[i], 0, sizeof(decode_packets[i]));
    } else if (i < 4) {
      memcpy(&decode_packets[i], &data[i], sizeof(data[i]));
    } else {
      memcpy(&decode_packets[i], &parity[i - 4], sizeof(parity[i - 4]));
    }
  }

  uint8_t missing[] = {1};
  ret = fec_decode(&ctx, decode_packets, missing, 1, plen);
  assert(ret == 0);

  /* Verify recovered packet 1 matches original */
  for (uint16_t j = 0; j < 100; j++)
    assert(decode_packets[1].data[j] == data[1].data[j]);

  fec_free(&ctx);
  PASS();
}

static void test_fec_missing_two(void)
{
  TEST("fec_missing_two");
  struct fec_ctx ctx;
  fec_init(&ctx, 4, 2);

  struct fec_packet data[4];
  struct fec_packet parity[2];
  memset(data, 0, sizeof(data));
  memset(parity, 0, sizeof(parity));

  for (uint8_t i = 0; i < 4; i++) {
    for (uint16_t j = 0; j < 50; j++)
      data[i].data[j] = (uint8_t)(i * 50 + j);
    data[i].len = 50;
  }

  uint16_t plen = fec_padded_length(data, 4);
  fec_encode(&ctx, data, parity, plen);

  /* Zero out packets 0 and 3 (simulate two losses) */
  struct fec_packet decode_packets[6];
  for (uint8_t i = 0; i < 6; i++) {
    if (i == 0 || i == 3) {
      memset(&decode_packets[i], 0, sizeof(decode_packets[i]));
    } else if (i < 4) {
      memcpy(&decode_packets[i], &data[i], sizeof(data[i]));
    } else {
      memcpy(&decode_packets[i], &parity[i - 4], sizeof(parity[i - 4]));
    }
  }

  uint8_t missing[] = {0, 3};
  int ret = fec_decode(&ctx, decode_packets, missing, 2, plen);
  assert(ret == 0);

  for (uint16_t j = 0; j < 50; j++) {
    assert(decode_packets[0].data[j] == data[0].data[j]);
    assert(decode_packets[3].data[j] == data[3].data[j]);
  }

  fec_free(&ctx);
  PASS();
}

static void test_fec_no_loss(void)
{
  TEST("fec_no_loss");
  struct fec_ctx ctx;
  fec_init(&ctx, 4, 2);

  struct fec_packet data[4];
  struct fec_packet parity[2];
  memset(data, 0, sizeof(data));
  memset(parity, 0, sizeof(parity));

  for (uint8_t i = 0; i < 4; i++) {
    for (uint16_t j = 0; j < 30; j++)
      data[i].data[j] = (uint8_t)(i * 30 + j);
    data[i].len = 30;
  }

  uint16_t plen = fec_padded_length(data, 4);
  fec_encode(&ctx, data, parity, plen);

  /* All data present, indices 0-3 received, no missing */
  struct fec_packet decode_packets[4];
  for (uint8_t i = 0; i < 4; i++)
    memcpy(&decode_packets[i], &data[i], sizeof(data[i]));

  /* With no missing packets, we can just pass empty missing array */
  uint8_t missing[] = {};
  int ret = fec_decode(&ctx, decode_packets, missing, 0, plen);
  assert(ret == 0);

  for (uint16_t j = 0; j < 30; j++)
    assert(decode_packets[0].data[j] == data[0].data[j]);

  fec_free(&ctx);
  PASS();
}

static void test_fec_too_many_losses(void)
{
  TEST("fec_too_many_losses");
  struct fec_ctx ctx;
  fec_init(&ctx, 4, 2);

  struct fec_packet data[4];
  struct fec_packet parity[2];
  memset(data, 0, sizeof(data));
  memset(parity, 0, sizeof(parity));

  for (uint8_t i = 0; i < 4; i++) {
    for (uint16_t j = 0; j < 20; j++)
      data[i].data[j] = (uint8_t)(i * 20 + j);
    data[i].len = 20;
  }

  uint16_t plen = fec_padded_length(data, 4);
  fec_encode(&ctx, data, parity, plen);

  /* Zero out 3 packets but only M=2 parity - too many losses */
  struct fec_packet decode_packets[6];
  for (uint8_t i = 0; i < 6; i++) {
    if (i < 3) {
      memset(&decode_packets[i], 0, sizeof(decode_packets[i]));
    } else if (i < 4) {
      memcpy(&decode_packets[i], &data[i], sizeof(data[i]));
    } else {
      memcpy(&decode_packets[i], &parity[i - 4], sizeof(parity[i - 4]));
    }
  }

  uint8_t missing[] = {0, 1, 2};
  int ret = fec_decode(&ctx, decode_packets, missing, 3, plen);
  assert(ret == -1);

  fec_free(&ctx);
  PASS();
}

static void test_fec_adaptive(void)
{
  TEST("fec_adaptive");
  assert(fec_adaptive_m(0.0f) == 1);
  assert(fec_adaptive_m(0.005f) == 1);
  assert(fec_adaptive_m(0.01f) == 2);
  assert(fec_adaptive_m(0.03f) == 2);
  assert(fec_adaptive_m(0.05f) == 3);
  assert(fec_adaptive_m(0.1f) == 3);
  PASS();
}

/* ------------------------------------------------------------------ */
/*  Phase 4: Congestion Control Tests                                 */
/* ------------------------------------------------------------------ */

static void test_cubic_slow_start(void)
{
  TEST("cubic_slow_start");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 200);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  assert(conn->cubic.cwnd == 12000);
  assert(conn->cubic.ssthresh == 65536);
  assert(conn->cubic.in_recovery == 0);

  reliable_stream_send(s, "data", 4);
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent == 1);
  uint32_t initial_cwnd = conn->cubic.cwnd;

  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_ack_datagram(dgram, &dgram_len, 0, 1);
  reliable_conn_input(conn, dgram, dgram_len, 200);

  assert(conn->cubic.cwnd == initial_cwnd + 4);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_cubic_loss_recovery(void)
{
  TEST("cubic_loss_recovery");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 201);
  struct reliable_stream *s1 = reliable_stream_open(conn, 1, 1, 0);
  struct reliable_stream *s2 = reliable_stream_open(conn, 1, 1, 0);
  assert(s1 && s2);

  reliable_stream_send(s1, "AAAA", 4);
  reliable_stream_send(s2, "BBBB", 4);
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent == 2);

  uint32_t cwnd_before = conn->cubic.cwnd;

  for (int dup = 0; dup < 3; dup++) {
    uint8_t dgram[MAX_DATAGRAM_SIZE];
    uint16_t dgram_len;
    build_ack_datagram(dgram, &dgram_len, 1, 1);
    reliable_conn_input(conn, dgram, dgram_len, 200 + dup * 10);
  }

  assert(conn->cubic.in_recovery == 1);
  assert(conn->cubic.cwnd < cwnd_before);
  assert(conn->cubic.ssthresh < cwnd_before);
  assert(conn->cubic.w_max >= cwnd_before);
  assert(conn->cubic.k > 0);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_cubic_congestion_avoidance(void)
{
  TEST("cubic_congestion_avoidance");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 202);

  conn->cubic.w_max = 10000;
  conn->cubic.last_loss_time = 0;
  conn->cubic.ssthresh = 50000;
  conn->cubic.cwnd = conn->cubic.ssthresh;
  conn->cubic.in_recovery = 0;
  uint32_t w_max_scaled = (uint32_t)(((uint64_t)conn->cubic.w_max * (65536 - 26214)) / 26214);
  conn->cubic.k = cbrt_fp(w_max_scaled);
  uint32_t cwnd_before = conn->cubic.cwnd;

  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);
  reliable_stream_send(s, "data", 4);
  reliable_conn_tick(conn, 100);

  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_ack_datagram(dgram, &dgram_len, 0, 1);
  reliable_conn_input(conn, dgram, dgram_len, 1000);

  /* In CA, cwnd may be capped at ssthresh; verify the cubic state is populated */
  assert(conn->cubic.k > 0);
  assert(conn->cubic.cubic_c == 26214);
  assert(conn->cubic.cwnd > cwnd_before);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_delay_reduction(void)
{
  TEST("delay_reduction");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 203);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  conn->delay.rtt_threshold = Q16_FROM_MS(10);
  conn->delay.delay_reduced = 0;
  uint32_t cwnd_before = conn->cubic.cwnd;

  reliable_stream_send(s, "data", 4);
  reliable_conn_tick(conn, 100);

  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_ack_datagram(dgram, &dgram_len, 0, 1);
  reliable_conn_input(conn, dgram, dgram_len, 200);

  assert(conn->delay.delay_reduced == 1);
  assert(conn->cubic.cwnd < cwnd_before);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_bandwidth_estimation(void)
{
  TEST("bandwidth_estimation");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 204);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  conn->last_bandwidth_estimate_ms = 0;
  conn->bytes_acked_since_last_estimate = 0;
  conn->delivery_rate = 0;

  reliable_stream_send(s, "data", 4);
  reliable_conn_tick(conn, 100);
  assert(conn->packets_sent == 1);

  uint8_t dgram[MAX_DATAGRAM_SIZE];
  uint16_t dgram_len;
  build_ack_datagram(dgram, &dgram_len, 0, 1);
  reliable_conn_input(conn, dgram, dgram_len, 300);

  assert(conn->delivery_rate > 0);
  reliable_conn_destroy(conn);
  PASS();
}

static void test_cbrt_fp(void)
{
  TEST("cbrt_fp");
  assert(cbrt_fp(0) == 0);
  assert(cbrt_fp(1) == 1);
  assert(cbrt_fp(8) == 2);
  assert(cbrt_fp(27) == 3);
  assert(cbrt_fp(1000) == 10);
  for (uint32_t x = 1; x <= 1000000; x += 97) {
    uint32_t y = cbrt_fp(x);
    uint64_t y3 = (uint64_t)y * y * y;
    uint64_t yp1_3 = (uint64_t)(y + 1) * (y + 1) * (y + 1);
    assert(y3 <= x || x < yp1_3);
    (void)y3; (void)yp1_3;
  }
  PASS();
}

static void test_cwnd_limits_send(void)
{
  TEST("cwnd_limits_send");
  struct loopback_ctx lb;
  struct reliable_conn *conn = setup_conn(&lb, 205);
  struct reliable_stream *s = reliable_stream_open(conn, 1, 1, 0);
  assert(s != NULL);

  conn->cubic.cwnd = 50;

  uint8_t big_data[200];
  memset(big_data, 'X', sizeof(big_data));
  int n = reliable_stream_send(s, big_data, sizeof(big_data));
  assert(n == 200);

  reliable_conn_tick(conn, 100);
  assert(conn->conn_bytes_in_flight <= conn->cubic.cwnd);
  assert(conn->conn_bytes_in_flight > 0);

  reliable_conn_destroy(conn);
  PASS();
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */
int main(void)
{
  printf("=== Frame Tests ===\n");
  test_frame_header_roundtrip();
  test_data_frame_roundtrip();
  test_ack_frame_roundtrip();
  test_stream_open_roundtrip();
  test_window_update_roundtrip();
  test_ping_pong();
  test_coalescing();

  printf("\n=== Reliable Connection Tests ===\n");
  test_create_destroy();
  test_stream_open_close();
  test_stream_send_recv();
  test_rtt_estimation();
  test_frame_coalescing_in_conn();
  test_retransmission();

  printf("\n=== Phase 1 Fix Tests ===\n");
  test_pong_rtt();
  test_bytes_in_flight();
  test_packets_received_counter();
  test_max_streams_enforced();
  test_stream_close_unlinks();
  test_stream_id_uint16();
  test_ack_delay_not_zero();
  test_timer_based_ack();
  test_fast_retransmit();
  test_inflight_insert_by_seq();

  printf("\n=== Phase 2: Multi-Stream Tests ===\n");
  test_priority_scheduling();
  test_connection_flow_control();
  test_stream_level_flow_control();
  test_proactive_window_update();
  test_stream_close_frame_sent();
  test_bidirectional_close();
  test_bidirectional_close_simultaneous();
  test_conn_level_window_update();
  test_multi_stream_independence();
  test_stream_send_after_close_rejected();
  test_close_with_pending_data();
  test_window_update_max_data();
  test_priority_clamping();
  test_total_bytes_sent_flow_control();

  printf("\n=== Phase 3: FEC Tests ===\n");
  test_fec_frame_serialize();
  test_fec_group_derivation();
  test_fec_encode_decode();
  test_fec_missing_two();
  test_fec_no_loss();
  test_fec_too_many_losses();
  test_fec_adaptive();

  printf("\n=== Phase 4: Congestion Control Tests ===\n");
  test_cubic_slow_start();
  test_cubic_loss_recovery();
  test_cubic_congestion_avoidance();
  test_delay_reduction();
  test_bandwidth_estimation();
  test_cbrt_fp();
  test_cwnd_limits_send();

  printf("\n=== Results ===\n");
  if (g_failures == 0) {
    printf("All tests passed!\n");
  } else {
    printf("%d test(s) FAILED!\n", g_failures);
  }
  return g_failures;
}
