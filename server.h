#ifndef SERVER_H
#define SERVER_H

#include "common.h"

#define MAX_EVENTS 64
#define CLIENT_TIMEOUT_SEC 30
#define RESET_RETRY_INTERVAL_SEC 1
#define RESET_MAX_RETRIES 3

struct server_context {
    int udp_fd;
    struct server_client *clients;
    uint32_t next_notify_seq;
};

int server_init(struct server_context *ctx, uint16_t port);
void server_run(struct server_context *ctx);
void server_cleanup(struct server_context *ctx);

void server_handle_message(struct server_context *ctx, const struct sockaddr_in *src_addr, const void *msg, size_t len);
void server_handle_login(struct server_context *ctx, const struct sockaddr_in *src_addr, const void *body, uint16_t type);
void server_handle_heartbeat(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct msg_header *hdr, const struct heartbeat_req *req);
void server_handle_punch_req(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct punch_req *req);
void server_handle_reset_punch(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct reset_punch *req);
void server_handle_reset_ack(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct msg_header *hdr);
void server_send_reset_notify(struct server_context *ctx, struct server_client *client, const char *peer_id, const struct punch_notify *peer_info);
void server_send_reset_notify_v2(struct server_context *ctx, struct server_client *client, const char *peer_id, const struct punch_notify_v2 *peer_info);
void server_check_timeouts(struct server_context *ctx);
struct server_client* server_find_client(struct server_context *ctx, const char *id);
void server_remove_client(struct server_context *ctx, struct server_client *client);

#endif /* SERVER_H */
