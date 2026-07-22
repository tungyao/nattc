#ifndef RELIABLE_UDP_H
#define RELIABLE_UDP_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h> /* struct sockaddr, socklen_t */
#include <netinet/in.h> /* struct sockaddr_in */

#include "frame.h"
#include "fec.h"

/* Default buffer sizes */
#define RELIABLE_STREAM_SEND_BUF_SIZE (64 * 1024)
#define RELIABLE_STREAM_RECV_BUF_SIZE (64 * 1024)

/* Maximum number of in-flight (sent but un-acked) packets */
#define MAX_INFLIGHT 256

/* Maximum number of streams per connection */
#define MAX_STREAMS 64

/* Maximum number of received packet seq numbers tracked for ACK generation */
#define PACKET_HISTORY_BITS 1024
#define PACKET_HISTORY_SIZE (PACKET_HISTORY_BITS / 8)

/* Maximum number of ACK ranges we can generate */
#define MAX_ACK_RANGES 16

/* Default timing constants */
#define DEFAULT_MAX_ACK_DELAY_MS 25
#define INITIAL_RTT_MS 200
#define PING_INTERVAL_MS 1000

/* Stream states */
enum stream_state {
  STREAM_OPEN = 0,
  STREAM_SEND_CLOSED,
  STREAM_CLOSED
};

/* Flow control state (per-stream) */
struct stream_flow_control {
  uint32_t send_window;
  uint32_t recv_window;
  uint32_t bytes_in_flight;
  uint32_t bytes_received;
};

/* RTT estimator using Q16.16 fixed-point for smoothed_rtt and rtt_variance */
struct rtt_estimator {
  uint32_t min_rtt;         /* in milliseconds */
  uint32_t smoothed_rtt;    /* Q16.16 fixed-point */
  uint32_t rtt_variance;    /* Q16.16 fixed-point */
  uint32_t latest_rtt;      /* in milliseconds */
};

/* An entry in the per-connection in-flight packet tracking table.
 * Indexed by (packet_seq % MAX_INFLIGHT). */
struct inflight_entry {
  uint32_t packet_seq;
  uint32_t stream_seq;
  uint32_t send_time_ms;
  uint16_t payload_len;
  uint16_t retransmits;
  uint16_t stream_id;
  uint16_t dup_ack_count;
  uint8_t reliable;
  uint8_t valid;
  uint8_t acked;
  uint8_t payload[MAX_DATAGRAM_SIZE];
};

/* Receive-side state for ACK generation */
struct ack_tracker {
  uint32_t base_seq;
  uint32_t max_seq;
  uint32_t last_received_time;
  uint32_t received_count;
  uint8_t bitmap[PACKET_HISTORY_SIZE];
};

/* A node in the stream's out-of-order receive list */
struct recv_data_node {
  uint32_t stream_seq;
  uint32_t len;
  uint8_t *data;
  struct recv_data_node *next;
};

/* A stream within a reliable connection */
struct reliable_stream {
  uint16_t stream_id;
  uint8_t reliable;
  uint8_t ordered;
  uint8_t priority;
  enum stream_state state;
  uint8_t sent_close_frame;
  uint8_t remote_closed;

  struct reliable_conn *conn;

  /* Send side */
  uint8_t send_buf[RELIABLE_STREAM_SEND_BUF_SIZE];
  uint32_t send_buf_head;
  uint32_t send_buf_tail;
  uint32_t next_stream_seq_send;
  uint32_t total_bytes_sent;

  /* Receive side */
  uint8_t recv_buf[RELIABLE_STREAM_RECV_BUF_SIZE];
  uint32_t recv_buf_head;
  uint32_t recv_buf_tail;
  uint32_t next_stream_seq_recv;
  struct recv_data_node *recv_pending;
  uint8_t pending_window_update;

  /* Flow control */
  struct stream_flow_control flow;

  /* In-stream pointer (linked list) */
  struct reliable_stream *next;
};

/* Callback to actually send data over the wire */
typedef int (*send_func_t)(void *ctx, const void *data, uint32_t len,
                            const struct sockaddr *addr, socklen_t addrlen);

/* Reliable connection */
struct reliable_conn {
  uint32_t session_id;
  send_func_t send_fn;
  void *send_ctx;

  /* Peer address */
  struct sockaddr_in peer_addr;

  /* Packet sequence numbers */
  uint32_t next_packet_seq;

  /* In-flight packet tracking */
  struct inflight_entry inflight[MAX_INFLIGHT];

  /* Receive-side ACK tracking */
  struct ack_tracker ack_tx; /* what we've received from peer */

  /* RTT estimation */
  struct rtt_estimator rtt;
  uint32_t max_ack_delay_ms;

  /* Streams */
  struct reliable_stream *streams;
  uint16_t next_initiator_stream_id;
  uint16_t next_responder_stream_id;
  int is_initiator;

  /* Connection-level flow control */
  uint32_t conn_bytes_in_flight;
  uint32_t conn_send_window;

  /* Stats for congestion (minimal for now) */
  uint32_t bytes_sent;
  uint32_t bytes_received;
  uint32_t packets_sent;
  uint32_t packets_received;
  uint32_t packets_lost;
  uint32_t packets_retransmitted;

  /* Time-based ACK generation */
  uint32_t last_ack_send_ms;
  int pending_ack;

  /* Conn tick state */
  uint32_t last_tick_ms;

  /* FEC state (Phase 3) */
  uint8_t fec_enabled;
  uint8_t fec_n;
  uint8_t fec_m;
  uint32_t fec_loss_counter;
  uint32_t fec_total_counter;
  struct fec_send_group fec_send;
  struct fec_recv_group fec_recv;

  /* Linked list of connections (if needed) */
  struct reliable_conn *next;
};

/* Public API */

/* Create a reliable connection.
 * session_id: opaque identifier for this connection.
 * send_fn: callback to send data.
 * send_ctx: context passed to send_fn. */
struct reliable_conn* reliable_conn_create(uint32_t session_id,
                                           send_func_t send_fn,
                                           void *send_ctx);

/* Destroy a reliable connection and all its streams */
void reliable_conn_destroy(struct reliable_conn *conn);

/* Set the peer address for sending */
void reliable_conn_set_peer(struct reliable_conn *conn,
                            const struct sockaddr_in *addr);

/* Open a new stream */
struct reliable_stream* reliable_stream_open(struct reliable_conn *conn,
                                              uint8_t reliable,
                                              uint8_t ordered,
                                              uint8_t priority);

/* Close a stream */
void reliable_stream_close(struct reliable_stream *stream);

/* Send data on a stream (copies data into send buffer) */
int reliable_stream_send(struct reliable_stream *stream,
                          const void *data, uint32_t len);

/* Receive data from a stream (non-blocking) */
int reliable_stream_recv(struct reliable_stream *stream,
                          void *buf, uint32_t buf_len);

/* Periodic tick: send pending data, process timeouts, send ACKs */
int reliable_conn_tick(struct reliable_conn *conn, uint32_t now_ms);

/* Process an incoming datagram */
int reliable_conn_input(struct reliable_conn *conn,
                         const void *pkt, uint32_t len,
                         uint32_t now_ms);

/* Access RTT information */
uint32_t reliable_conn_get_smoothed_rtt_ms(struct reliable_conn *conn);
uint32_t reliable_conn_get_min_rtt_ms(struct reliable_conn *conn);

/* Helper: get current time in milliseconds (wrapper around get_time_ms) */
uint32_t reliable_time_ms(void);

#endif /* RELIABLE_UDP_H */
