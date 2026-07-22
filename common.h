#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#pragma comment(lib, "ws2_32.lib")
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

/* Pack all network protocol structs to ensure consistent layout across architectures */
/* GCC, Clang, and MSVC all support #pragma pack */
#pragma pack(push, 1)

/* Magic number for protocol identification */
#define PROTO_MAGIC 0xCAFE

/* Message types */
#define MSG_LOGIN          0x01
#define MSG_LOGIN_RESP     0x02
#define MSG_PUNCH_REQ      0x03
#define MSG_PUNCH_NOTIFY   0x04
#define MSG_RESET_PUNCH    0x05
#define MSG_RESET_NOTIFY   0x06
#define MSG_P2P_DATA       0x07
#define MSG_HEARTBEAT      0x08
#define MSG_HEARTBEAT_RESP 0x09
#define MSG_RESET_ACK      0x0A
#define MSG_PUNCH_ECHO     0x0B
#define MSG_PUNCH_ACK      0x0C
#define MSG_LOGIN_V2       0x0D
#define MSG_LOGIN_RESP_V2  0x0E
#define MSG_PUNCH_NOTIFY_V2 0x0F
#define MSG_RESET_NOTIFY_V2 0x10

/* Message header */
struct msg_header {
    uint16_t magic;
    uint16_t type;
    uint32_t seq;
    uint32_t body_len;
};

/* Login request body */
struct login_req {
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
};

/* Login response body */
struct login_resp {
    struct sockaddr_in public_addr;
    uint32_t client_count;
};

struct client_info {
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
    struct sockaddr_in public_addr;
};

/* Heartbeat request body */
struct heartbeat_req {
    char id[32];
};

/* Punch request body */
struct punch_req {
    char id[32];
    char target_id[32];
};

/* Punch notify body */
struct punch_notify {
    char peer_id[32];
    uint32_t peer_vip;
    uint8_t peer_mac[6];
    struct sockaddr_in peer_public;
};

/* Reset punch body (request) */
struct reset_punch {
    char id[32];
    char peer_id[32];
};

/* Reset notify body (from server) */
struct reset_notify {
    uint32_t notify_seq;
    char peer_id[32];
    uint32_t peer_vip;
    uint8_t peer_mac[6];
    struct sockaddr_in peer_new_public;
    uint32_t new_session_id;
};

/* P2P data header */
struct p2p_data_header {
    uint32_t session_id;
    uint32_t seq;
};

/* V2 login request */
struct login_req_v2 {
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
    struct sockaddr_in local_addr;
};

/* V2 login response body */
struct login_resp_v2 {
    struct sockaddr_in public_addr;
    uint32_t client_count;
};

/* V2 client info */
struct client_info_v2 {
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
    struct sockaddr_in public_addr;
    struct sockaddr_in local_addr;
};

/* V2 punch notify */
struct punch_notify_v2 {
    char peer_id[32];
    uint32_t peer_vip;
    uint8_t peer_mac[6];
    struct sockaddr_in peer_public;
    struct sockaddr_in peer_local;
};

/* V2 reset notify */
struct reset_notify_v2 {
    uint32_t notify_seq;
    char peer_id[32];
    uint32_t peer_vip;
    uint8_t peer_mac[6];
    struct sockaddr_in peer_new_public;
    struct sockaddr_in peer_new_local;
    uint32_t new_session_id;
};

/* Punch echo body */
struct punch_echo {
    char peer_id[32];
    uint32_t session_id;
};

/* Punch ack body */
struct punch_ack {
    uint32_t session_id;
};

/* Heartbeat response body */
struct heartbeat_resp {
    struct sockaddr_in public_addr;
};

#pragma pack(pop)

/* Peer states */
enum peer_state {
    PEER_STATE_IDLE = 0,
    PEER_STATE_PUNCHING,
    PEER_STATE_ESTABLISHED,
    PEER_STATE_RESETTING
};

/* ARP table entry */
struct arp_entry {
    uint32_t vip;
    uint8_t mac[6];
    char peer_id[32];
    time_t last_seen;
    struct arp_entry *next;
};

struct reliable_conn;

/* Peer session */
struct peer_session {
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
    struct sockaddr_in public_addr;
    struct sockaddr_in local_addr;
    enum peer_state state;
    uint32_t session_id;
    uint32_t tx_seq;
    uint32_t rx_seq;
    time_t last_rx_time;
    time_t last_tx_time;
    int punch_attempts;
    time_t last_punch_time;
    int lan_phase;
    uint32_t reset_notify_seq;
    int reset_ack_received;
    int reset_retries;
    time_t last_reset_time;
    struct reliable_conn *rconn;
    struct peer_session *next;
};

/* Server client entry */
struct server_client {
    char id[32];
    uint32_t vip;
    uint8_t mac[6];
    struct sockaddr_in public_addr;
    struct sockaddr_in local_addr;
    time_t last_heartbeat;
    struct server_client *next;
};

/* Utility macros */
#define MSG_HEADER_SIZE sizeof(struct msg_header)
#define MAX_MSG_SIZE    2000

/* Function declarations for utils */
int create_udp_socket(void);
int set_nonblocking(int fd);
int send_msg(int fd, const struct sockaddr_in *addr, uint16_t type, uint32_t seq, const void *body, uint32_t body_len);
int recv_msg(int fd, struct sockaddr_in *addr, struct msg_header *hdr, void *body, uint32_t *body_len);
char* sock_strerror(void);
int sock_errno(void);
void print_addr(const struct sockaddr_in *addr);

/* Cross-platform close/closesocket */
#ifdef _WIN32
#define sock_close(fd) closesocket(fd)
#else
#define sock_close(fd) close(fd)
#endif

#endif /* COMMON_H */
