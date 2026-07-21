#ifndef CLIENT_H
#define CLIENT_H

#include "common.h"

#define MAX_EVENTS 64
#define PUNCH_ATTEMPTS_FIRST 5
#define PUNCH_INTERVAL_FIRST_MS 200
#define PUNCH_INTERVAL_MS 1000
#define PUNCH_TIMEOUT_SEC 30
#define LAN_PUNCH_ATTEMPTS_MAX 5
#define LAN_PUNCH_INTERVAL_MS 200
#define LAN_PUNCH_TIMEOUT_SEC 3
#define PEER_TIMEOUT_SEC 15
#define PEER_KEEPALIVE_INTERVAL 5
#define HEARTBEAT_INTERVAL_SEC 10
#define LOGIN_TIMEOUT_SEC 5
#define PUNCH_REBIND_THRESHOLD 2
#define TUN_MTU 1400

enum lan_punch_phase {
    LAN_PHASE_NONE = 0,
    LAN_PHASE_LAN,
    LAN_PHASE_WAN
};

#ifdef _WIN32
/* Wintun adapter handles */
struct wintun_ctx {
    void *adapter;
    void *session;
    void *read_event;
};
#endif

/* Client context */
struct client_context {
    int tun_fd;           /* POSIX: TUN fd; Windows: -1 (use Wintun) */
    int udp_fd;
    char tun_name[16];
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
    struct sockaddr_in server_addr;
    struct sockaddr_in local_addr;
    struct sockaddr_in server_observed_addr;
    struct arp_entry *arp_table;
    struct peer_session *peers;
    time_t last_heartbeat;
    time_t login_sent_time;
    int login_received;
    int punch_failures;
#ifdef _WIN32
    struct wintun_ctx wintun;
#endif
};

int client_init(struct client_context *ctx, const char *server_ip, uint16_t server_port,
                const char *client_id, const char *virtual_ip);
void client_run(struct client_context *ctx);
void client_cleanup(struct client_context *ctx);

/* TUN device management */
int tun_create(struct client_context *ctx, const char *dev_name);
int tun_set_ip(struct client_context *ctx, const char *ip, const char *netmask);
int tun_set_mtu(struct client_context *ctx, int mtu);
int tun_set_route(struct client_context *ctx, const char *dst, const char *gw);
int tun_del_route(struct client_context *ctx, const char *dst);
int tun_write(struct client_context *ctx, const void *data, uint32_t len);
int tun_read(struct client_context *ctx, void *buf, uint32_t len);

/* ARP table management */
struct arp_entry* arp_find(struct client_context *ctx, uint32_t vip);
void arp_add(struct client_context *ctx, uint32_t vip, const uint8_t *mac, const char *peer_id);
void arp_remove(struct client_context *ctx, uint32_t vip);
void arp_clear_all(struct client_context *ctx);
void arp_clear_peer(struct client_context *ctx, const char *peer_id);

/* Peer session management */
struct peer_session* peer_find(struct client_context *ctx, const char *id);
struct peer_session* peer_find_by_vip(struct client_context *ctx, uint32_t vip);
struct peer_session* peer_add(struct client_context *ctx, const char *id, uint32_t vip, const uint8_t *mac);
void peer_remove(struct client_context *ctx, struct peer_session *peer);
void peer_clear_all_mappings(struct client_context *ctx, const char *peer_id, uint32_t vip);

/* Message handling */
void client_handle_message(struct client_context *ctx, const struct sockaddr_in *src_addr, const void *msg, size_t len);
void client_handle_login_resp(struct client_context *ctx, const struct login_resp *resp, uint32_t body_len, uint16_t type);
void client_handle_login_resp_v2(struct client_context *ctx, const struct login_resp_v2 *resp, uint32_t body_len);
void client_handle_punch_notify(struct client_context *ctx, const struct punch_notify *notify, uint16_t type);
void client_handle_punch_notify_v2(struct client_context *ctx, const struct punch_notify_v2 *notify);
void client_handle_reset_notify(struct client_context *ctx, const struct reset_notify *notify, uint16_t type);
void client_handle_reset_notify_v2(struct client_context *ctx, const struct reset_notify_v2 *notify);
void client_handle_punch_echo(struct client_context *ctx, const struct sockaddr_in *src_addr, const struct punch_echo *echo);
void client_handle_punch_ack(struct client_context *ctx, const struct punch_ack *ack);
void client_handle_p2p_data(struct client_context *ctx, const struct sockaddr_in *src_addr, const struct p2p_data_header *hdr, const void *data, uint32_t data_len);
void client_handle_heartbeat_resp(struct client_context *ctx, const struct heartbeat_resp *resp);

/* Punching logic */
void client_start_punching(struct client_context *ctx, struct peer_session *peer);
void client_send_punch_echo(struct client_context *ctx, struct peer_session *peer);
void client_update_punch_state(struct client_context *ctx, struct peer_session *peer);

/* Data transmission */
void client_send_p2p_data(struct client_context *ctx, struct peer_session *peer, const void *data, uint32_t len);
void client_process_tun_packet(struct client_context *ctx, const void *packet, uint32_t len);

/* ARP proxy */
void client_send_arp_reply(struct client_context *ctx, const uint8_t *pkt, uint32_t len);

/* Heartbeat */
void client_send_heartbeat(struct client_context *ctx);

/* Reset handling */
void client_initiate_reset(struct client_context *ctx, struct peer_session *peer);

/* Keepalive */
void client_send_keepalive(struct client_context *ctx, struct peer_session *peer);

/* LAN address discovery */
void client_discover_local_addr(struct client_context *ctx);
int is_rfc1918(struct in_addr addr);

/* UDP socket rebind */
int client_rebind_udp(struct client_context *ctx);
void client_resend_login(struct client_context *ctx);

#endif /* CLIENT_H */
