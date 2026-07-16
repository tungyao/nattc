#include "client.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <net/route.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
/* Wintun API declarations (loaded at runtime) */
typedef void* (*WintunCreateAdapter_t)(const wchar_t*, const wchar_t*, const void*);
typedef void* (*WintunStartSession_t)(void*, uint32_t);
typedef void  (*WintunEndSession_t)(void*);
typedef void  (*WintunCloseAdapter_t)(void*);
typedef void* (*WintunGetReadEvent_t)(void*);
typedef uint8_t* (*WintunReceivePacket_t)(void*, uint32_t*);
typedef void  (*WintunReleaseReceivePacket_t)(void*, const uint8_t*);
typedef uint8_t* (*WintunAllocateSendPacket_t)(void*, uint32_t);
typedef void  (*WintunSendPacket_t)(void*, const uint8_t*);
#endif

/* Wintun GUID for our adapter */
#ifdef _WIN32
static const GUID WINTUN_GUID = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}};
#endif

int client_init(struct client_context *ctx, const char *server_ip, uint16_t server_port,
                const char *client_id, const char *virtual_ip) {
    ctx->tun_fd = -1;
    ctx->udp_fd = -1;
#ifdef _WIN32
    memset(&ctx->wintun, 0, sizeof(ctx->wintun));
#endif
    memset(ctx->id, 0, sizeof(ctx->id));
    strncpy(ctx->id, client_id, sizeof(ctx->id) - 1);
    ctx->vip = ip_str_to_uint32(virtual_ip);
    ctx->arp_table = NULL;
    ctx->peers = NULL;
    ctx->last_heartbeat = 0;

#ifndef _WIN32
    srand(time(NULL) ^ getpid());
#else
    srand(time(NULL) ^ GetCurrentProcessId());
#endif
    ctx->mac[0] = 0x02;
    ctx->mac[1] = rand() & 0xFF;
    ctx->mac[2] = rand() & 0xFF;
    ctx->mac[3] = rand() & 0xFF;
    ctx->mac[4] = rand() & 0xFF;
    ctx->mac[5] = rand() & 0xFF;

    memset(&ctx->server_addr, 0, sizeof(ctx->server_addr));
    ctx->server_addr.sin_family = AF_INET;
    ctx->server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &ctx->server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        return -1;
    }

    char tun_name[16];
    snprintf(tun_name, sizeof(tun_name), "tun_%s", client_id);
    strncpy(ctx->tun_name, tun_name, sizeof(ctx->tun_name) - 1);
    if (tun_create(ctx, tun_name) < 0) {
        fprintf(stderr, "Failed to create TUN device\n");
        return -1;
    }

#ifndef _WIN32
    if (set_nonblocking(ctx->tun_fd) < 0) {
        fprintf(stderr, "Failed to set TUN non-blocking\n");
        sock_close(ctx->tun_fd);
        return -1;
    }
#endif

    if (tun_set_ip(ctx, virtual_ip, "255.255.255.255") < 0) {
        fprintf(stderr, "Failed to set TUN IP\n");
        sock_close(ctx->tun_fd);
        return -1;
    }

    if (tun_set_mtu(ctx, TUN_MTU) < 0) {
        fprintf(stderr, "Failed to set TUN MTU\n");
        sock_close(ctx->tun_fd);
        return -1;
    }

    ctx->udp_fd = create_udp_socket();
    if (ctx->udp_fd < 0) {
        fprintf(stderr, "Failed to create UDP socket\n");
        sock_close(ctx->tun_fd);
        return -1;
    }

    if (set_nonblocking(ctx->udp_fd) < 0) {
        fprintf(stderr, "Failed to set non-blocking\n");
        sock_close(ctx->udp_fd);
        sock_close(ctx->tun_fd);
        return -1;
    }

    printf("Client initialized: id=%s vip=%s\n", client_id, virtual_ip);
    return 0;
}

/* ============ TUN implementations ============ */

int tun_create(struct client_context *ctx, const char *dev_name) {
#ifndef _WIN32
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { perror("open /dev/net/tun"); return -1; }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev_name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("ioctl TUNSETIFF"); close(fd); return -1;
    }
    ctx->tun_fd = fd;
    printf("TUN device %s created\n", dev_name);
    return 0;
#else
    HMODULE wintun = LoadLibraryW(L"wintun.dll");
    if (!wintun) {
        fprintf(stderr, "Failed to load wintun.dll (error %lu). "
                "Download from https://www.wintun.net/ and place wintun.dll next to the executable.\n",
                GetLastError());
        return -1;
    }
    WintunCreateAdapter_t WintunCreateAdapter =
        (WintunCreateAdapter_t)GetProcAddress(wintun, "WintunCreateAdapter");
    WintunStartSession_t WintunStartSession =
        (WintunStartSession_t)GetProcAddress(wintun, "WintunStartSession");
    if (!WintunCreateAdapter || !WintunStartSession) {
        fprintf(stderr, "Invalid wintun.dll\n"); return -1;
    }

    wchar_t wname[64];
    mbstowcs(wname, dev_name, 64);
    ctx->wintun.adapter = WintunCreateAdapter(wname, L"P2P-VPN", &WINTUN_GUID);
    if (!ctx->wintun.adapter) {
        fprintf(stderr, "WintunCreateAdapter failed\n"); return -1;
    }
    ctx->wintun.session = WintunStartSession(ctx->wintun.adapter, 0x400000);
    if (!ctx->wintun.session) {
        fprintf(stderr, "WintunStartSession failed\n");
        WintunCloseAdapter_t WintunCloseAdapter =
            (WintunCloseAdapter_t)GetProcAddress(wintun, "WintunCloseAdapter");
        if (WintunCloseAdapter) WintunCloseAdapter(ctx->wintun.adapter);
        return -1;
    }
    WintunGetReadEvent_t WintunGetReadEvent =
        (WintunGetReadEvent_t)GetProcAddress(wintun, "WintunGetReadEvent");
    if (WintunGetReadEvent) {
        ctx->wintun.read_event = WintunGetReadEvent(ctx->wintun.session);
    }
    ctx->tun_fd = -1;
    printf("Wintun adapter %s created, read_event=%p\n", dev_name, ctx->wintun.read_event);
    return 0;
#endif
}

int tun_set_ip(struct client_context *ctx, const char *ip, const char *netmask) {
#ifndef _WIN32
    int fd;
    struct ifreq ifr;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ctx->tun_name, IFNAMSIZ - 1);
    struct sockaddr_in *addr = (struct sockaddr_in*)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) { perror("ioctl SIOCSIFADDR"); close(fd); return -1; }
    inet_pton(AF_INET, netmask, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) { perror("ioctl SIOCSIFNETMASK"); close(fd); return -1; }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) { perror("ioctl SIOCSIFFLAGS"); close(fd); return -1; }
    close(fd);
    printf("TUN IP set to %s/%s\n", ip, netmask);
    return 0;
#else
    char cmd[256];
    char adapter_id[32];
    snprintf(adapter_id, sizeof(adapter_id), "tun_%s", ctx->id);
    snprintf(cmd, sizeof(cmd),
             "netsh interface ipv4 set address name=\"%s\" source=static addr=%s mask=%s gateway=none",
             adapter_id, ip, netmask);
    int ret = system(cmd);
    if (ret != 0) fprintf(stderr, "netsh set address failed (code %d)\n", ret);
    printf("Wintun IP set via netsh: %s/%s\n", ip, netmask);
    return 0;
#endif
}

int tun_set_mtu(struct client_context *ctx, int mtu) {
#ifndef _WIN32
    int fd;
    struct ifreq ifr;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ctx->tun_name, IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;
    if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) {
        perror("ioctl SIOCSIFMTU"); close(fd); return -1;
    }
    close(fd);
    printf("TUN MTU set to %d\n", mtu);
    return 0;
#else
    char cmd[256];
    char adapter_id[32];
    snprintf(adapter_id, sizeof(adapter_id), "tun_%s", ctx->id);
    snprintf(cmd, sizeof(cmd),
             "netsh interface ipv4 set subinterface \"%s\" mtu=%d store=active",
             adapter_id, mtu);
    int ret = system(cmd);
    if (ret != 0) fprintf(stderr, "netsh set mtu failed (code %d)\n", ret);
    printf("Wintun MTU set to %d\n", mtu);
    return 0;
#endif
}

int tun_set_route(struct client_context *ctx, const char *dst, const char *gw) {
#ifndef _WIN32
    int fd;
    struct sockaddr_in *addr;
    struct rtentry route;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    memset(&route, 0, sizeof(route));
    addr = (struct sockaddr_in*)&route.rt_dst;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, dst, &addr->sin_addr);
    addr = (struct sockaddr_in*)&route.rt_genmask;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(0xFFFFFFFF);
    route.rt_dev = ctx->tun_name;
    route.rt_flags = RTF_UP | RTF_HOST;
    if (ioctl(fd, SIOCADDRT, &route) < 0) { perror("ioctl SIOCADDRT"); close(fd); return -1; }
    close(fd);
    printf("Route added: %s via %s\n", dst, ctx->tun_name);
    return 0;
#else
    char cmd[512];
    char adapter_id[32];
    snprintf(adapter_id, sizeof(adapter_id), "tun_%s", ctx->id);
    snprintf(cmd, sizeof(cmd),
             "route delete %s mask 255.255.255.255 2>nul & powershell -Command \"New-NetRoute -InterfaceAlias '%s' -DestinationPrefix '%s/32' -RouteMetric 1 -ErrorAction SilentlyContinue\"",
             dst, adapter_id, dst);
    int ret = system(cmd);
    if (ret != 0) fprintf(stderr, "route add failed (code %d)\n", ret);
    printf("Route added: %s dev %s\n", dst, adapter_id);
    return 0;
#endif
}

int tun_del_route(struct client_context *ctx, const char *dst) {
#ifndef _WIN32
    int fd;
    struct sockaddr_in *addr;
    struct rtentry route;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    memset(&route, 0, sizeof(route));
    addr = (struct sockaddr_in*)&route.rt_dst;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, dst, &addr->sin_addr);
    addr = (struct sockaddr_in*)&route.rt_genmask;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(0xFFFFFFFF);
    route.rt_dev = ctx->tun_name;
    route.rt_flags = RTF_UP | RTF_HOST;
    if (ioctl(fd, SIOCDELRT, &route) < 0) { perror("ioctl SIOCDELRT"); close(fd); return -1; }
    close(fd);
    printf("Route deleted: %s\n", dst);
    return 0;
#else
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "route delete %s mask 255.255.255.255 2>nul", dst);
    int ret = system(cmd);
    printf("Route deleted: %s (code %d)\n", dst, ret);
    return 0;
#endif
}

int tun_write(struct client_context *ctx, const void *data, uint32_t len) {
#ifndef _WIN32
    ssize_t written = write(ctx->tun_fd, data, len);
    if (written < 0) { perror("write TUN"); return -1; }
    return (int)written;
#else
    /* Wintun: allocate and send */
    HMODULE wintun = GetModuleHandleW(L"wintun.dll");
    if (!wintun) return -1;
    WintunAllocateSendPacket_t alloc = (WintunAllocateSendPacket_t)GetProcAddress(wintun, "WintunAllocateSendPacket");
    WintunSendPacket_t send = (WintunSendPacket_t)GetProcAddress(wintun, "WintunSendPacket");
    if (!alloc || !send) return -1;
    uint8_t *packet = alloc(ctx->wintun.session, len);
    if (!packet) return -1;
    memcpy(packet, data, len);
    send(ctx->wintun.session, packet);
    return (int)len;
#endif
}

int tun_read(struct client_context *ctx, void *buf, uint32_t len) {
#ifndef _WIN32
    ssize_t n = read(ctx->tun_fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        perror("read TUN");
        return -1;
    }
    return (int)n;
#else
    HMODULE wintun = GetModuleHandleW(L"wintun.dll");
    if (!wintun) return -1;
    WintunReceivePacket_t recv = (WintunReceivePacket_t)GetProcAddress(wintun, "WintunReceivePacket");
    WintunReleaseReceivePacket_t release = (WintunReleaseReceivePacket_t)GetProcAddress(wintun, "WintunReleaseReceivePacket");
    if (!recv || !release) return -1;
    uint32_t pkt_len;
    uint8_t *packet = recv(ctx->wintun.session, &pkt_len);
    if (!packet) return -2;
    if (pkt_len <= len) memcpy(buf, packet, pkt_len);
    release(ctx->wintun.session, packet);
    return (int)pkt_len;
#endif
}

/* ============ ARP table ============ */

struct arp_entry* arp_find(struct client_context *ctx, uint32_t vip) {
    struct arp_entry *entry = ctx->arp_table;
    while (entry) {
        if (entry->vip == vip) return entry;
        entry = entry->next;
    }
    return NULL;
}

void arp_add(struct client_context *ctx, uint32_t vip, const uint8_t *mac, const char *peer_id) {
    struct arp_entry *entry = arp_find(ctx, vip);
    if (entry) {
        memcpy(entry->mac, mac, 6);
        strncpy(entry->peer_id, peer_id, sizeof(entry->peer_id) - 1);
        entry->peer_id[sizeof(entry->peer_id) - 1] = '\0';
        entry->last_seen = time(NULL);
        return;
    }
    entry = (struct arp_entry*)malloc(sizeof(struct arp_entry));
    if (!entry) { perror("malloc"); return; }
    entry->vip = vip;
    memcpy(entry->mac, mac, 6);
    strncpy(entry->peer_id, peer_id, sizeof(entry->peer_id) - 1);
    entry->peer_id[sizeof(entry->peer_id) - 1] = '\0';
    entry->last_seen = time(NULL);
    entry->next = ctx->arp_table;
    ctx->arp_table = entry;

    char ip_str[INET_ADDRSTRLEN];
    ip_uint32_to_str(vip, ip_str, sizeof(ip_str));
    printf("ARP add: %s -> %02x:%02x:%02x:%02x:%02x:%02x (peer %s)\n",
           ip_str, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], peer_id);

    char vip_str[INET_ADDRSTRLEN];
    ip_uint32_to_str(vip, vip_str, sizeof(vip_str));
    tun_set_route(ctx, vip_str, NULL);
}

void arp_remove(struct client_context *ctx, uint32_t vip) {
    struct arp_entry **pp = &ctx->arp_table;
    while (*pp) {
        if ((*pp)->vip == vip) {
            struct arp_entry *entry = *pp;
            *pp = entry->next;
            char vip_str[INET_ADDRSTRLEN];
            ip_uint32_to_str(vip, vip_str, sizeof(vip_str));
            tun_del_route(ctx, vip_str);
            free(entry);
            return;
        }
        pp = &(*pp)->next;
    }
}

void arp_clear_all(struct client_context *ctx) {
    struct arp_entry *entry = ctx->arp_table;
    while (entry) {
        struct arp_entry *next = entry->next;
        char vip_str[INET_ADDRSTRLEN];
        ip_uint32_to_str(entry->vip, vip_str, sizeof(vip_str));
        tun_del_route(ctx, vip_str);
        free(entry);
        entry = next;
    }
    ctx->arp_table = NULL;
}

void arp_clear_peer(struct client_context *ctx, const char *peer_id) {
    struct arp_entry **pp = &ctx->arp_table;
    while (*pp) {
        if (strcmp((*pp)->peer_id, peer_id) == 0) {
            struct arp_entry *entry = *pp;
            *pp = entry->next;
            char vip_str[INET_ADDRSTRLEN];
            ip_uint32_to_str(entry->vip, vip_str, sizeof(vip_str));
            tun_del_route(ctx, vip_str);
            free(entry);
        } else {
            pp = &(*pp)->next;
        }
    }
}

/* ============ Peer session management ============ */

struct peer_session* peer_find(struct client_context *ctx, const char *id) {
    struct peer_session *peer = ctx->peers;
    while (peer) {
        if (strcmp(peer->id, id) == 0) return peer;
        peer = peer->next;
    }
    return NULL;
}

struct peer_session* peer_find_by_vip(struct client_context *ctx, uint32_t vip) {
    struct peer_session *peer = ctx->peers;
    while (peer) {
        if (peer->vip == vip) return peer;
        peer = peer->next;
    }
    return NULL;
}

struct peer_session* peer_add(struct client_context *ctx, const char *id, uint32_t vip, const uint8_t *mac) {
    struct peer_session *peer = (struct peer_session*)calloc(1, sizeof(struct peer_session));
    if (!peer) { perror("calloc"); return NULL; }
    strncpy(peer->id, id, sizeof(peer->id) - 1);
    peer->id[sizeof(peer->id) - 1] = '\0';
    peer->vip = vip;
    if (mac) memcpy(peer->mac, mac, 6);
    peer->state = PEER_STATE_IDLE;
    peer->next = ctx->peers;
    ctx->peers = peer;
    printf("Peer added: %s\n", id);
    return peer;
}

void peer_remove(struct client_context *ctx, struct peer_session *peer) {
    struct peer_session **pp = &ctx->peers;
    while (*pp) {
        if (*pp == peer) { *pp = peer->next; free(peer); return; }
        pp = &(*pp)->next;
    }
}

void peer_clear_all_mappings(struct client_context *ctx, const char *peer_id, uint32_t vip) {
    struct peer_session *peer = peer_find(ctx, peer_id);
    if (!peer) return;
    printf("Clearing all mappings for peer %s\n", peer_id);
    arp_clear_peer(ctx, peer_id);
    peer->state = PEER_STATE_RESETTING;
    peer->tx_seq = 0;
    peer->rx_seq = 0;
    peer->punch_attempts = 0;
    peer->last_punch_time = 0;
    peer->reset_ack_received = 0;
    peer->reset_retries = 0;
    peer->last_reset_time = 0;
}

/* ============ Main event loop ============ */

extern volatile int running;

void client_run(struct client_context *ctx) {
    time_t last_timeout_check = time(NULL);

    /* Send login to server */
    struct login_req login;
    memset(&login, 0, sizeof(login));
    strncpy(login.id, ctx->id, sizeof(login.id) - 1);
    login.vip = ctx->vip;
    memcpy(login.mac, ctx->mac, 6);
    send_msg(ctx->udp_fd, &ctx->server_addr, MSG_LOGIN, 0, &login, sizeof(login));
    ctx->last_heartbeat = time(NULL);
    ctx->login_sent_time = time(NULL);
    ctx->login_received = 0;

    printf("Client running...\n");

    while (running) {
        struct timeval tv;
        fd_set readfds;
        int max_fd = ctx->udp_fd;

        FD_ZERO(&readfds);
        FD_SET(ctx->udp_fd, &readfds);

#ifndef _WIN32
        FD_SET(ctx->tun_fd, &readfds);
        if (ctx->tun_fd > max_fd) max_fd = ctx->tun_fd;
        tv.tv_sec = 0; tv.tv_usec = 100000;
#else
        tv.tv_sec = 0; tv.tv_usec = 50000;
#endif

        int nfds = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (nfds < 0) {
            if (sock_errno() == EINTR
#ifdef _WIN32
                || sock_errno() == WSAEINTR
#endif
               ) continue;
            fprintf(stderr, "select failed: %s\n", sock_strerror());
            break;
        }

        /* Handle UDP data */
        if (FD_ISSET(ctx->udp_fd, &readfds)) {
            for (;;) {
                char buf[MAX_MSG_SIZE];
                struct sockaddr_in src_addr;
#ifdef _WIN32
                int addrlen = sizeof(src_addr);
#else
                socklen_t addrlen = sizeof(src_addr);
#endif
                ssize_t n = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0,
                                     (struct sockaddr*)&src_addr, &addrlen);
                if (n < 0) {
                    int se = sock_errno();
                    if (se == EAGAIN
#ifdef _WIN32
                        || se == WSAEWOULDBLOCK
#else
                        || se == EWOULDBLOCK
#endif
                       ) break;
                    fprintf(stderr, "recvfrom: %s\n", sock_strerror());
                    break;
                }
                if (n == 0) break;
                client_handle_message(ctx, &src_addr, buf, (size_t)n);
            }
        }

        /* Handle TUN data */
#ifndef _WIN32
        if (FD_ISSET(ctx->tun_fd, &readfds)) {
            for (;;) {
                char buf[MAX_MSG_SIZE];
                int n = tun_read(ctx, buf, sizeof(buf));
                if (n == -2) break;
                if (n < 0) break;
                if (n == 0) break;
                client_process_tun_packet(ctx, buf, (uint32_t)n);
            }
        }
#else
        /* Windows: poll Wintun non-blocking */
        {
            int should_read = 1;
            if (ctx->wintun.read_event) {
                DWORD wr = WaitForSingleObject(ctx->wintun.read_event, 0);
                if (wr == WAIT_OBJECT_0) {
                    /* event signaled, read below */
                } else if (wr == WAIT_TIMEOUT) {
                    should_read = 0; /* no data ready */
                } else {
                    static int once = 0;
                    if (!once) { fprintf(stderr, "WaitForSingleObject FAILED: %lu\n", GetLastError()); once = 1; }
                    should_read = 0;
                }
            }
            /* If read_event is NULL or signaled, try non-blocking read */
            if (should_read) {
                for (;;) {
                    char buf[MAX_MSG_SIZE];
                    int n = tun_read(ctx, buf, sizeof(buf));
                    if (n == -2) break;
                    if (n < 0) break;
                    if (n == 0) break;
                    printf("TUN recv: %d bytes\n", n);
                    client_process_tun_packet(ctx, buf, (uint32_t)n);
                }
            }
        }
#endif

        /* Periodic tasks */
        time_t now = time(NULL);
        if (!ctx->login_received && (now - ctx->login_sent_time) > LOGIN_TIMEOUT_SEC) {
            fprintf(stderr, "ERROR: Cannot reach server (no response after %d seconds).\n"
                    "       Check server IP/port, firewall rules, and that the server is running.\n",
                    LOGIN_TIMEOUT_SEC);
            running = 0;
            break;
        }
        if (now - last_timeout_check >= 1) {
            if (now - ctx->last_heartbeat >= HEARTBEAT_INTERVAL_SEC) {
                client_send_heartbeat(ctx);
                ctx->last_heartbeat = now;
            }
            struct peer_session *peer = ctx->peers;
            while (peer) {
                if (peer->state == PEER_STATE_ESTABLISHED) {
                    if (peer->last_rx_time > 0 && (now - peer->last_rx_time) > PEER_TIMEOUT_SEC) {
                        printf("Peer %s timed out, initiating reset\n", peer->id);
                        client_initiate_reset(ctx, peer);
                    } else if (peer->last_tx_time > 0 && 
                               (now - peer->last_tx_time) > PEER_KEEPALIVE_INTERVAL) {
                        client_send_keepalive(ctx, peer);
                    }
                } else if (peer->state == PEER_STATE_PUNCHING || peer->state == PEER_STATE_RESETTING) {
                    client_update_punch_state(ctx, peer);
                }
                peer = peer->next;
            }
            /* Print periodic peer status summary every 5 seconds */
            if (ctx->peers && (now % 5) == 1) {
                printf("\n========== Peer Status Summary ==========\n");
                struct peer_session *p = ctx->peers;
                while (p) {
                    const char *state_str;
                    char vip_str[INET_ADDRSTRLEN];
                    ip_uint32_to_str(p->vip, vip_str, sizeof(vip_str));
                    switch (p->state) {
                        case PEER_STATE_IDLE:        state_str = "IDLE";        break;
                        case PEER_STATE_PUNCHING:    state_str = "PUNCHING";    break;
                        case PEER_STATE_ESTABLISHED: state_str = "ESTABLISHED"; break;
                        case PEER_STATE_RESETTING:   state_str = "RESETTING";   break;
                        default:                     state_str = "UNKNOWN";     break;
                    }
                    printf("  Peer %-8s VIP=%-16s State=%-12s Attempts=%d\n",
                           p->id, vip_str, state_str, p->punch_attempts);
                    p = p->next;
                }
                printf("========================================\n\n");
            }
            last_timeout_check = now;
        }
    }
}

void client_cleanup(struct client_context *ctx) {
    struct peer_session *peer = ctx->peers;
    while (peer) {
        struct peer_session *next = peer->next;
        free(peer);
        peer = next;
    }
    ctx->peers = NULL;
    arp_clear_all(ctx);

#ifndef _WIN32
    if (ctx->tun_fd >= 0) sock_close(ctx->tun_fd);
#else
    if (ctx->wintun.session) {
        HMODULE wintun = GetModuleHandleW(L"wintun.dll");
        if (wintun) {
            WintunEndSession_t endSess = (WintunEndSession_t)GetProcAddress(wintun, "WintunEndSession");
            WintunCloseAdapter_t closeAd = (WintunCloseAdapter_t)GetProcAddress(wintun, "WintunCloseAdapter");
            if (endSess) endSess(ctx->wintun.session);
            if (closeAd) closeAd(ctx->wintun.adapter);
        }
    }
#endif
    if (ctx->udp_fd >= 0) sock_close(ctx->udp_fd);
}

/* ============ Message handling ============ */

void client_handle_message(struct client_context *ctx, const struct sockaddr_in *src_addr, const void *msg, size_t len) {
    if (len < sizeof(struct msg_header)) { fprintf(stderr, "Message too short\n"); return; }
    const struct msg_header *hdr = (const struct msg_header*)msg;
    uint16_t magic = ntohs(hdr->magic);
    uint16_t type = ntohs(hdr->type);
    uint32_t body_len = ntohl(hdr->body_len);
    if (magic != PROTO_MAGIC) { fprintf(stderr, "Invalid magic: 0x%04X\n", magic); return; }
    if (len < sizeof(struct msg_header) + body_len) { fprintf(stderr, "Message truncated\n"); return; }
    const void *body = (const char*)msg + sizeof(struct msg_header);
    switch (type) {
        case MSG_LOGIN_RESP:   client_handle_login_resp(ctx, (const struct login_resp*)body, body_len); break;
        case MSG_PUNCH_NOTIFY: client_handle_punch_notify(ctx, (const struct punch_notify*)body); break;
        case MSG_RESET_NOTIFY: client_handle_reset_notify(ctx, (const struct reset_notify*)body); break;
        case MSG_PUNCH_ECHO:   client_handle_punch_echo(ctx, src_addr, (const struct punch_echo*)body); break;
        case MSG_PUNCH_ACK:    client_handle_punch_ack(ctx, (const struct punch_ack*)body); break;
        case MSG_P2P_DATA:
            client_handle_p2p_data(ctx, src_addr, (const struct p2p_data_header*)body,
                                   (const char*)body + sizeof(struct p2p_data_header),
                                   body_len - sizeof(struct p2p_data_header));
            break;
        case MSG_HEARTBEAT_RESP: client_handle_heartbeat_resp(ctx, (const struct heartbeat_resp*)body); break;
        default: fprintf(stderr, "Unknown message type: 0x%04X\n", type); break;
    }
}

void client_handle_login_resp(struct client_context *ctx, const struct login_resp *resp, uint32_t body_len) {
    ctx->login_received = 1;
    printf("Login response: server observed address ");
    print_addr(&resp->public_addr);
    printf("\n");
    uint32_t count = ntohl(resp->client_count);
    const struct client_info *info = (const struct client_info*)(resp + 1);
    for (uint32_t i = 0; i < count; i++) {
        printf("  Known peer: %s vip=%08x\n", info[i].id, info[i].vip);
        arp_add(ctx, info[i].vip, info[i].mac, info[i].id);
        struct peer_session *peer = peer_find(ctx, info[i].id);
        if (!peer) peer = peer_add(ctx, info[i].id, info[i].vip, info[i].mac);
        if (peer) peer->public_addr = info[i].public_addr;
        printf("  Requesting punch for peer %s\n", info[i].id);
        struct punch_req req;
        memset(&req, 0, sizeof(req));
        strncpy(req.target_id, info[i].id, sizeof(req.target_id) - 1);
        send_msg(ctx->udp_fd, &ctx->server_addr, MSG_PUNCH_REQ, 0, &req, sizeof(req));
    }
}

void client_handle_punch_notify(struct client_context *ctx, const struct punch_notify *notify) {
    printf("Punch notify: peer %s at ", notify->peer_id);
    print_addr(&notify->peer_public);
    printf("\n");
    if (notify->peer_public.sin_addr.s_addr == 0 && notify->peer_public.sin_port == 0) {
        printf("Peer %s is offline\n", notify->peer_id);
        struct peer_session *peer = peer_find(ctx, notify->peer_id);
        if (peer && (peer->state == PEER_STATE_PUNCHING || peer->state == PEER_STATE_RESETTING))
            peer->state = PEER_STATE_IDLE;
        return;
    }
    struct peer_session *peer = peer_find(ctx, notify->peer_id);
    if (!peer) { peer = peer_add(ctx, notify->peer_id, notify->peer_vip, notify->peer_mac); if (!peer) return; }
    peer->public_addr = notify->peer_public;
    if (notify->peer_vip) peer->vip = notify->peer_vip;
    memcpy(peer->mac, notify->peer_mac, 6);
    arp_add(ctx, notify->peer_vip, notify->peer_mac, notify->peer_id);

    if (peer->state == PEER_STATE_ESTABLISHED) {
        printf("Peer %s reconnected (was established), re-punching\n", notify->peer_id);
        peer_clear_all_mappings(ctx, notify->peer_id, notify->peer_vip);
        arp_add(ctx, notify->peer_vip, notify->peer_mac, notify->peer_id);
        client_start_punching(ctx, peer);
        return;
    }
    if (peer->state == PEER_STATE_IDLE) {
        client_start_punching(ctx, peer);
    }
}

void client_handle_reset_notify(struct client_context *ctx, const struct reset_notify *notify) {
    uint32_t notify_seq = ntohl(notify->notify_seq);
    printf("Reset notify: peer %s seq=%u\n", notify->peer_id, notify_seq);
    struct peer_session *peer = peer_find(ctx, notify->peer_id);
    if (!peer) { peer = peer_add(ctx, notify->peer_id, notify->peer_vip, notify->peer_mac); if (!peer) return; }
    if (peer->reset_notify_seq == notify_seq && peer->state == PEER_STATE_PUNCHING) {
        send_msg(ctx->udp_fd, &ctx->server_addr, MSG_RESET_ACK, notify_seq, NULL, 0);
        return;
    }
    peer_clear_all_mappings(ctx, notify->peer_id, notify->peer_vip);
    if (notify->peer_new_public.sin_addr.s_addr != 0)
        peer->public_addr = notify->peer_new_public;
    if (notify->peer_vip) peer->vip = notify->peer_vip;
    memcpy(peer->mac, notify->peer_mac, 6);
    peer->session_id = ntohl(notify->new_session_id);
    peer->reset_notify_seq = notify_seq;
    arp_add(ctx, notify->peer_vip, notify->peer_mac, notify->peer_id);
    send_msg(ctx->udp_fd, &ctx->server_addr, MSG_RESET_ACK, notify_seq, NULL, 0);
    if (notify->peer_new_public.sin_addr.s_addr != 0)
        client_start_punching(ctx, peer);
    else
        peer->state = PEER_STATE_IDLE;
}

void client_handle_punch_echo(struct client_context *ctx, const struct sockaddr_in *src_addr, const struct punch_echo *echo) {
    uint32_t session_id = ntohl(echo->session_id);
    printf("Punch echo from "); print_addr(src_addr);
    printf(" peer=%s session=%u\n", echo->peer_id, session_id);
    struct peer_session *peer = peer_find(ctx, echo->peer_id);
    if (!peer) { printf("Unknown peer: %s\n", echo->peer_id); return; }
    peer->public_addr = *src_addr;
    peer->last_rx_time = time(NULL);

    if (peer->state == PEER_STATE_ESTABLISHED) {
        printf("Punch echo from %s while established, updating session\n", echo->peer_id);
        if (session_id > peer->session_id) {
            peer->session_id = session_id;
            printf("Session updated to %u\n", session_id);
        }
        struct punch_ack ack;
        ack.session_id = htonl(peer->session_id);
        send_msg(ctx->udp_fd, src_addr, MSG_PUNCH_ACK, 0, &ack, sizeof(ack));
        peer->last_tx_time = time(NULL);
        return;
    }
    if (peer->state != PEER_STATE_PUNCHING && peer->state != PEER_STATE_RESETTING) {
        printf("Unexpected punch echo in state %d\n", peer->state);
        return;
    }
    if (session_id > peer->session_id) peer->session_id = session_id;
    struct punch_ack ack;
    ack.session_id = htonl(peer->session_id);
    send_msg(ctx->udp_fd, src_addr, MSG_PUNCH_ACK, 0, &ack, sizeof(ack));
    peer->state = PEER_STATE_ESTABLISHED;
    peer->punch_attempts = 0;
    fprintf(stderr, "*** PUNCH SUCCEEDED with peer %s (session=%u) ***\n",
            peer->id, peer->session_id);
    struct punch_echo our_echo;
    strncpy(our_echo.peer_id, ctx->id, sizeof(our_echo.peer_id) - 1);
    our_echo.peer_id[sizeof(our_echo.peer_id) - 1] = '\0';
    our_echo.session_id = htonl(peer->session_id);
    send_msg(ctx->udp_fd, &peer->public_addr, MSG_PUNCH_ECHO, 0, &our_echo, sizeof(our_echo));
    printf("Sent post-establish echo to %s with session=%u\n", peer->id, peer->session_id);
}

void client_handle_punch_ack(struct client_context *ctx, const struct punch_ack *ack) {
    uint32_t ack_session = ntohl(ack->session_id);
    printf("Punch ACK received, session=%u\n", ack_session);
    struct peer_session *peer = ctx->peers;
    while (peer) {
        if (peer->state == PEER_STATE_PUNCHING || peer->state == PEER_STATE_RESETTING) {
            if (ack_session > peer->session_id) peer->session_id = ack_session;
            peer->state = PEER_STATE_ESTABLISHED;
            peer->last_rx_time = time(NULL);
            peer->punch_attempts = 0;
            fprintf(stderr, "*** PUNCH ACK — session established with %s (session=%u) ***\n",
                    peer->id, peer->session_id);
            struct punch_echo our_echo;
            strncpy(our_echo.peer_id, ctx->id, sizeof(our_echo.peer_id) - 1);
            our_echo.peer_id[sizeof(our_echo.peer_id) - 1] = '\0';
            our_echo.session_id = htonl(peer->session_id);
            send_msg(ctx->udp_fd, &peer->public_addr, MSG_PUNCH_ECHO, 0, &our_echo, sizeof(our_echo));
            printf("Sent post-ACK echo to %s with session=%u\n", peer->id, peer->session_id);
            return;
        }
        peer = peer->next;
    }
    printf("Punch ACK: no peer in punching state\n");
}

void client_handle_p2p_data(struct client_context *ctx, const struct sockaddr_in *src_addr,
                            const struct p2p_data_header *hdr, const void *data, uint32_t data_len) {
    uint32_t session_id = ntohl(hdr->session_id);
    uint32_t seq = ntohl(hdr->seq);
    printf("P2P data: session=%u seq=%u len=%u from ", session_id, seq, data_len);
    print_addr(src_addr);
    printf("\n");

    struct peer_session *peer = ctx->peers;
    while (peer) {
        if (peer->session_id == session_id && peer->state == PEER_STATE_ESTABLISHED) break;
        peer = peer->next;
    }
    if (!peer) { fprintf(stderr, "P2P data for unknown session %u\n", session_id); return; }
    if (seq <= peer->rx_seq && peer->rx_seq != 0) {
        fprintf(stderr, "Out-of-order/duplicate: seq=%u, expected>%u\n", seq, peer->rx_seq);
        return;
    }
    peer->rx_seq = seq;
    peer->last_rx_time = time(NULL);
    if (peer->public_addr.sin_addr.s_addr != src_addr->sin_addr.s_addr ||
        peer->public_addr.sin_port != src_addr->sin_port) {
        printf("Peer %s address updated: ", peer->id);
        print_addr(src_addr);
        printf("\n");
        peer->public_addr = *src_addr;
    }
    int written = tun_write(ctx, data, data_len);
    if (written < 0) {
        fprintf(stderr, "write TUN failed: %s\n", sock_strerror());
    } else {
        printf("P2P data: wrote %d bytes to TUN (session=%u)\n", written, session_id);
    }
}

void client_handle_heartbeat_resp(struct client_context *ctx, const struct heartbeat_resp *resp) {
    printf("Heartbeat response: server sees us at ");
    print_addr(&resp->public_addr);
    printf("\n");
}

/* ============ Punching logic ============ */

void client_start_punching(struct client_context *ctx, struct peer_session *peer) {
    printf("Starting punch to %s at ", peer->id);
    print_addr(&peer->public_addr);
    printf("\n");
    peer->state = PEER_STATE_PUNCHING;
    peer->punch_attempts = 0;
    peer->last_punch_time = 0;
    peer->last_rx_time = time(NULL);
    peer->session_id = rand();
    client_send_punch_echo(ctx, peer);
}

void client_send_punch_echo(struct client_context *ctx, struct peer_session *peer) {
    struct punch_echo echo;
    strncpy(echo.peer_id, ctx->id, sizeof(echo.peer_id) - 1);
    echo.peer_id[sizeof(echo.peer_id) - 1] = '\0';
    echo.session_id = htonl(peer->session_id);
    printf("Sending punch echo to %s at ", peer->id);
    print_addr(&peer->public_addr);
    printf(" (attempt %d)\n", peer->punch_attempts + 1);
    send_msg(ctx->udp_fd, &peer->public_addr, MSG_PUNCH_ECHO, 0, &echo, sizeof(echo));
    peer->punch_attempts++;
    peer->last_punch_time = time(NULL);
}

void client_update_punch_state(struct client_context *ctx, struct peer_session *peer) {
    time_t now = time(NULL);
    if (peer->state == PEER_STATE_PUNCHING || peer->state == PEER_STATE_RESETTING) {
        if ((now - peer->last_rx_time) > PUNCH_TIMEOUT_SEC) {
            fprintf(stderr, "*** PUNCH TIMEOUT for peer %s after %d attempts ***\n",
                    peer->id, peer->punch_attempts);
            peer->state = PEER_STATE_IDLE;
            return;
        }
        int interval_ms = (peer->punch_attempts < PUNCH_ATTEMPTS_FIRST)
                          ? PUNCH_INTERVAL_FIRST_MS : PUNCH_INTERVAL_MS;
        long elapsed_ms = (long)(now - peer->last_punch_time) * 1000;
        if (elapsed_ms >= interval_ms) client_send_punch_echo(ctx, peer);
    }
}

/* ============ Data transmission ============ */

void client_send_p2p_data(struct client_context *ctx, struct peer_session *peer, const void *data, uint32_t len) {
    if (peer->state != PEER_STATE_ESTABLISHED) {
        fprintf(stderr, "Cannot send to %s: not established\n", peer->id);
        return;
    }
    size_t total_len = sizeof(struct p2p_data_header) + len;
    char *buf = (char*)malloc(total_len);
    if (!buf) { perror("malloc"); return; }
    struct p2p_data_header *hdr = (struct p2p_data_header*)buf;
    hdr->session_id = htonl(peer->session_id);
    peer->tx_seq++;
    hdr->seq = htonl(peer->tx_seq);
    memcpy(buf + sizeof(struct p2p_data_header), data, len);
    send_msg(ctx->udp_fd, &peer->public_addr, MSG_P2P_DATA, 0, buf, total_len);
    peer->last_tx_time = time(NULL);
    free(buf);
}

void client_process_tun_packet(struct client_context *ctx, const void *packet, uint32_t len) {
    if (len < 20) { fprintf(stderr, "TUN: packet too short (%u)\n", len); return; }
    const uint8_t *pkt = (const uint8_t*)packet;
    /* Check for IPv4 */
    if (((pkt[0] >> 4) & 0x0F) == 4) {
        uint32_t dst_ip;
        memcpy(&dst_ip, pkt + 16, 4);
        char dst_str[INET_ADDRSTRLEN];
        ip_uint32_to_str(dst_ip, dst_str, sizeof(dst_str));
        printf("TUN: IPv4 proto=%u dst=%s\n", pkt[9], dst_str);
        struct arp_entry *arp = arp_find(ctx, dst_ip);
        if (!arp) {
            if (dst_ip == ctx->vip) {
                printf("TUN: local VIP packet, writing to TUN\n");
                tun_write(ctx, packet, len);
                return;
            }
            fprintf(stderr, "TUN: *** No ARP entry for %s ***\n", dst_str);
            return;
        }
        printf("TUN: ARP hit peer=%s\n", arp->peer_id);
        struct peer_session *peer = peer_find(ctx, arp->peer_id);
        if (!peer) { fprintf(stderr, "TUN: No peer session for %s\n", arp->peer_id); return; }
        if (peer->state != PEER_STATE_ESTABLISHED) {
            fprintf(stderr, "TUN: *** Peer %s state=%d (need %d=ESTABLISHED) — DROPPING packet ***\n",
                    peer->id, peer->state, PEER_STATE_ESTABLISHED);
            return;
        }
        printf("TUN: Sending %u bytes to peer %s\n", len, peer->id);
        client_send_p2p_data(ctx, peer, packet, len);
        return;
    }
    /* Non-IPv4 packet: check if it looks like an Ethernet frame (TAP mode) with ARP */
    /* Ethernet frames: dstMAC(6)+srcMAC(6)+EtherType(2), ARP EtherType=0x0806 */
    if (len >= 42 && pkt[12] == 0x08 && pkt[13] == 0x06) {
        printf("TUN: ARP packet detected (EtherType 0x0806), handling via ARP proxy\n");
        client_send_arp_reply(ctx, packet, len);
    } else {
        printf("TUN: non-IPv4/non-ARP packet (first byte 0x%02x, len=%u), discarding\n", pkt[0], len);
    }
}

/* ============ ARP proxy ============ */

void client_send_arp_reply(struct client_context *ctx, const uint8_t *pkt, uint32_t len) {
    if (len < 28) return;
    uint16_t op = ntohs(*(uint16_t*)(pkt + 6));
    if (op != 1) return;
    const uint8_t *target_ip_ptr = pkt + 24;
    uint32_t target_ip;
    memcpy(&target_ip, target_ip_ptr, 4);
    struct arp_entry *arp = arp_find(ctx, target_ip);
    if (!arp) return;
    uint8_t reply[42];
    memset(reply, 0, sizeof(reply));
    reply[0] = arp->mac[0]; reply[1] = arp->mac[1]; reply[2] = arp->mac[2];
    reply[3] = arp->mac[3]; reply[4] = arp->mac[4]; reply[5] = arp->mac[5];
    reply[6] = ctx->mac[0]; reply[7] = ctx->mac[1]; reply[8] = ctx->mac[2];
    reply[9] = ctx->mac[3]; reply[10] = ctx->mac[4]; reply[11] = ctx->mac[5];
    reply[12] = 0x08; reply[13] = 0x06;
    reply[14] = 0x00; reply[15] = 0x01;
    reply[16] = 0x08; reply[17] = 0x00;
    reply[18] = 0x00; reply[19] = 0x06;
    reply[20] = 0x00; reply[21] = 0x04;
    reply[22] = 0x00; reply[23] = 0x02;
    memcpy(reply + 22, arp->mac, 6);
    memcpy(reply + 28, &target_ip, 4);
    memcpy(reply + 32, ctx->mac, 6);
    uint32_t sender_ip;
    memcpy(&sender_ip, pkt + 14, 4);
    memcpy(reply + 38, &sender_ip, 4);
    tun_write(ctx, reply, 42);
    char ip_str[INET_ADDRSTRLEN];
    ip_uint32_to_str(target_ip, ip_str, sizeof(ip_str));
    printf("ARP proxy reply: %s is at %02x:%02x:%02x:%02x:%02x:%02x\n",
           ip_str, arp->mac[0], arp->mac[1], arp->mac[2], arp->mac[3], arp->mac[4], arp->mac[5]);
}

/* ============ Heartbeat ============ */

void client_send_heartbeat(struct client_context *ctx) {
    send_msg(ctx->udp_fd, &ctx->server_addr, MSG_HEARTBEAT, 0, NULL, 0);
}

/* ============ Keepalive ============ */

void client_send_keepalive(struct client_context *ctx, struct peer_session *peer) {
    if (peer->state != PEER_STATE_ESTABLISHED) return;
    /* Send PUNCH_ECHO as lightweight keepalive to keep NAT mapping alive */
    peer->last_tx_time = time(NULL);
    struct punch_echo echo;
    strncpy(echo.peer_id, ctx->id, sizeof(echo.peer_id) - 1);
    echo.peer_id[sizeof(echo.peer_id) - 1] = '\0';
    echo.session_id = htonl(peer->session_id);
    send_msg(ctx->udp_fd, &peer->public_addr, MSG_PUNCH_ECHO, 0, &echo, sizeof(echo));
}

void client_initiate_reset(struct client_context *ctx, struct peer_session *peer) {
    printf("Initiating reset for peer %s\n", peer->id);
    peer_clear_all_mappings(ctx, peer->id, peer->vip);
    struct reset_punch req;
    memset(&req, 0, sizeof(req));
    strncpy(req.peer_id, peer->id, sizeof(req.peer_id) - 1);
    send_msg(ctx->udp_fd, &ctx->server_addr, MSG_RESET_PUNCH, 0, &req, sizeof(req));
}
