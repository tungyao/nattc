#include "server.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <errno.h>
#endif

int server_init(struct server_context *ctx, uint16_t port) {
    ctx->udp_fd = -1;
    ctx->clients = NULL;
    ctx->next_notify_seq = 1;

    ctx->udp_fd = create_udp_socket();
    if (ctx->udp_fd < 0) { fprintf(stderr, "Failed to create UDP socket\n"); return -1; }

    if (set_nonblocking(ctx->udp_fd) < 0) {
        fprintf(stderr, "Failed to set non-blocking\n");
        sock_close(ctx->udp_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(ctx->udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind failed: %s\n", sock_strerror());
        sock_close(ctx->udp_fd);
        return -1;
    }

    printf("Server initialized on port %d\n", port);
    return 0;
}

extern volatile int running;

void server_run(struct server_context *ctx) {
    time_t last_timeout_check = time(NULL);
    printf("Server running...\n");

    while (running) {
        struct timeval tv;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->udp_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int nfds = select(ctx->udp_fd + 1, &readfds, NULL, NULL, &tv);
        if (nfds < 0) {
            if (sock_errno() == EINTR
#ifdef _WIN32
                || sock_errno() == WSAEINTR
#endif
               ) continue;
            fprintf(stderr, "select failed: %s\n", sock_strerror());
            break;
        }

        if (FD_ISSET(ctx->udp_fd, &readfds)) {
            char buf[MAX_MSG_SIZE];
            struct sockaddr_in src_addr;
#ifdef _WIN32
            int addrlen = sizeof(src_addr);
#else
            socklen_t addrlen = sizeof(src_addr);
#endif
            ssize_t n = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&src_addr, &addrlen);
            if (n > 0) {
                server_handle_message(ctx, &src_addr, buf, (size_t)n);
            }
        }

        time_t now = time(NULL);
        if (now - last_timeout_check >= 1) {
            server_check_timeouts(ctx);
            last_timeout_check = now;
        }
    }
}

void server_cleanup(struct server_context *ctx) {
    struct server_client *client = ctx->clients;
    while (client) {
        struct server_client *next = client->next;
        free(client);
        client = next;
    }
    ctx->clients = NULL;
    if (ctx->udp_fd >= 0) sock_close(ctx->udp_fd);
}

void server_handle_message(struct server_context *ctx, const struct sockaddr_in *src_addr, const void *msg, size_t len) {
    if (len < sizeof(struct msg_header)) { fprintf(stderr, "Message too short\n"); return; }
    const struct msg_header *hdr = (const struct msg_header*)msg;
    uint16_t magic = ntohs(hdr->magic);
    uint16_t type = ntohs(hdr->type);
    uint32_t body_len = ntohl(hdr->body_len);
    if (magic != PROTO_MAGIC) { fprintf(stderr, "Invalid magic: 0x%04X\n", magic); return; }
    if (len < sizeof(struct msg_header) + body_len) { fprintf(stderr, "Message truncated\n"); return; }
    const void *body = (const char*)msg + sizeof(struct msg_header);
    switch (type) {
        case MSG_LOGIN:       server_handle_login(ctx, src_addr, (const struct login_req*)body); break;
        case MSG_HEARTBEAT:   server_handle_heartbeat(ctx, src_addr, hdr); break;
        case MSG_PUNCH_REQ:   server_handle_punch_req(ctx, src_addr, (const struct punch_req*)body); break;
        case MSG_RESET_PUNCH: server_handle_reset_punch(ctx, src_addr, (const struct reset_punch*)body); break;
        case MSG_RESET_ACK:   server_handle_reset_ack(ctx, src_addr, hdr); break;
        default: fprintf(stderr, "Unknown message type: 0x%04X\n", type); break;
    }
}

void server_handle_login(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct login_req *req) {
    printf("Login request from "); print_addr(src_addr); printf(" id=%s\n", req->id);
    struct server_client *client = server_find_client(ctx, req->id);
    if (client) {
        client->public_addr = *src_addr;
        client->vip = req->vip;
        memcpy(client->mac, req->mac, 6);
        client->last_heartbeat = time(NULL);
    } else {
        client = (struct server_client*)malloc(sizeof(struct server_client));
        if (!client) { perror("malloc"); return; }
        strncpy(client->id, req->id, sizeof(client->id) - 1);
        client->vip = req->vip;
        memcpy(client->mac, req->mac, 6);
        client->public_addr = *src_addr;
        client->last_heartbeat = time(NULL);
        client->next = ctx->clients;
        ctx->clients = client;
    }

    struct login_resp resp;
    resp.public_addr = *src_addr;
    uint32_t count = 0;
    struct server_client *c = ctx->clients;
    while (c) { if (strcmp(c->id, req->id) != 0) count++; c = c->next; }
    resp.client_count = htonl(count);

    size_t body_size = sizeof(resp) + count * sizeof(struct client_info);
    char *body = (char*)malloc(body_size);
    if (!body) { perror("malloc"); return; }
    memcpy(body, &resp, sizeof(resp));
    struct client_info *info = (struct client_info*)(body + sizeof(resp));
    c = ctx->clients;
    while (c) {
        if (strcmp(c->id, req->id) != 0) {
            strncpy(info->id, c->id, sizeof(info->id) - 1);
            info->vip = c->vip;
            memcpy(info->mac, c->mac, 6);
            info->public_addr = c->public_addr;
            info++;
        }
        c = c->next;
    }
    send_msg(ctx->udp_fd, src_addr, MSG_LOGIN_RESP, 0, body, body_size);
    free(body);
}

void server_handle_heartbeat(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct msg_header *hdr) {
    struct server_client *client = ctx->clients;
    while (client) {
        if (client->public_addr.sin_addr.s_addr == src_addr->sin_addr.s_addr &&
            client->public_addr.sin_port == src_addr->sin_port) {
            client->last_heartbeat = time(NULL);
            client->public_addr = *src_addr;
            struct heartbeat_resp resp;
            resp.public_addr = *src_addr;
            send_msg(ctx->udp_fd, src_addr, MSG_HEARTBEAT_RESP, hdr->seq, &resp, sizeof(resp));
            return;
        }
        client = client->next;
    }
    fprintf(stderr, "Heartbeat from unknown client "); print_addr(src_addr); fprintf(stderr, "\n");
}

void server_handle_punch_req(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct punch_req *req) {
    printf("Punch request from "); print_addr(src_addr); printf(" target=%s\n", req->target_id);
    struct server_client *requester = NULL;
    struct server_client *c = ctx->clients;
    while (c) {
        if (c->public_addr.sin_addr.s_addr == src_addr->sin_addr.s_addr &&
            c->public_addr.sin_port == src_addr->sin_port) { requester = c; break; }
        c = c->next;
    }
    if (!requester) { fprintf(stderr, "Requester not found\n"); return; }
    struct server_client *target = server_find_client(ctx, req->target_id);
    if (!target) { fprintf(stderr, "Target not found: %s\n", req->target_id); return; }

    struct punch_notify notify_target;
    strncpy(notify_target.peer_id, target->id, sizeof(notify_target.peer_id) - 1);
    notify_target.peer_vip = target->vip;
    memcpy(notify_target.peer_mac, target->mac, 6);
    notify_target.peer_public = target->public_addr;
    send_msg(ctx->udp_fd, src_addr, MSG_PUNCH_NOTIFY, 0, &notify_target, sizeof(notify_target));

    struct punch_notify notify_requester;
    strncpy(notify_requester.peer_id, requester->id, sizeof(notify_requester.peer_id) - 1);
    notify_requester.peer_vip = requester->vip;
    memcpy(notify_requester.peer_mac, requester->mac, 6);
    notify_requester.peer_public = requester->public_addr;
    send_msg(ctx->udp_fd, &target->public_addr, MSG_PUNCH_NOTIFY, 0, &notify_requester, sizeof(notify_requester));
}

void server_handle_reset_punch(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct reset_punch *req) {
    printf("Reset punch request from "); print_addr(src_addr); printf(" peer=%s\n", req->peer_id);
    struct server_client *requester = NULL;
    struct server_client *c = ctx->clients;
    while (c) {
        if (c->public_addr.sin_addr.s_addr == src_addr->sin_addr.s_addr &&
            c->public_addr.sin_port == src_addr->sin_port) { requester = c; break; }
        c = c->next;
    }
    if (!requester) { fprintf(stderr, "Requester not found\n"); return; }
    struct server_client *target = server_find_client(ctx, req->peer_id);
    if (!target) {
        struct punch_notify zero_notify;
        memset(&zero_notify, 0, sizeof(zero_notify));
        strncpy(zero_notify.peer_id, req->peer_id, sizeof(zero_notify.peer_id) - 1);
        server_send_reset_notify(ctx, requester, req->peer_id, &zero_notify);
        return;
    }
    struct punch_notify notify_target;
    strncpy(notify_target.peer_id, target->id, sizeof(notify_target.peer_id) - 1);
    notify_target.peer_vip = target->vip;
    memcpy(notify_target.peer_mac, target->mac, 6);
    notify_target.peer_public = target->public_addr;
    server_send_reset_notify(ctx, requester, req->peer_id, &notify_target);

    struct punch_notify notify_requester;
    strncpy(notify_requester.peer_id, requester->id, sizeof(notify_requester.peer_id) - 1);
    notify_requester.peer_vip = requester->vip;
    memcpy(notify_requester.peer_mac, requester->mac, 6);
    notify_requester.peer_public = requester->public_addr;
    server_send_reset_notify(ctx, target, requester->id, &notify_requester);
}

void server_handle_reset_ack(struct server_context *ctx, const struct sockaddr_in *src_addr, const struct msg_header *hdr) {
    printf("Reset ACK from "); print_addr(src_addr); printf(" seq=%u\n", ntohl(hdr->seq));
    (void)ctx;
}

void server_send_reset_notify(struct server_context *ctx, struct server_client *client, const char *peer_id, const struct punch_notify *peer_info) {
    struct reset_notify notify;
    notify.notify_seq = htonl(ctx->next_notify_seq++);
    strncpy(notify.peer_id, peer_id, sizeof(notify.peer_id) - 1);
    notify.peer_vip = peer_info->peer_vip;
    memcpy(notify.peer_mac, peer_info->peer_mac, 6);
    notify.peer_new_public = peer_info->peer_public;
    notify.new_session_id = htonl(rand());
    printf("Sending reset notify to "); print_addr(&client->public_addr);
    printf(" seq=%u\n", ntohl(notify.notify_seq));
    send_msg(ctx->udp_fd, &client->public_addr, MSG_RESET_NOTIFY, 0, &notify, sizeof(notify));
}

void server_check_timeouts(struct server_context *ctx) {
    time_t now = time(NULL);
    struct server_client *prev = NULL;
    struct server_client *client = ctx->clients;
    while (client) {
        struct server_client *next = client->next;
        if (now - client->last_heartbeat > CLIENT_TIMEOUT_SEC) {
            printf("Client %s timed out\n", client->id);
            if (prev) prev->next = next;
            else      ctx->clients = next;
            free(client);
            client = next;
            continue;
        }
        prev = client;
        client = next;
    }
}

struct server_client* server_find_client(struct server_context *ctx, const char *id) {
    struct server_client *client = ctx->clients;
    while (client) {
        if (strcmp(client->id, id) == 0) return client;
        client = client->next;
    }
    return NULL;
}

void server_remove_client(struct server_context *ctx, struct server_client *client) {
    struct server_client *prev = NULL;
    struct server_client *c = ctx->clients;
    while (c) {
        if (c == client) {
            if (prev) prev->next = c->next;
            else      ctx->clients = c->next;
            free(c);
            return;
        }
        prev = c;
        c = c->next;
    }
}
