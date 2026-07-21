#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

int create_udp_socket(void) {
    int fd;
#ifdef _WIN32
    fd = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (fd == INVALID_SOCKET) {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        return -1;
    }
    {
        DWORD bytes = 0;
        BOOL disable = FALSE;
        WSAIoctl((SOCKET)fd, SIO_UDP_CONNRESET, &disable, sizeof(disable),
                 NULL, 0, &bytes, NULL, NULL);
    }
#else
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { perror("socket"); return -1; }
#endif
    int buf_size = 262144;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
    return fd;
}

int set_nonblocking(int fd) {
#ifdef _WIN32
    unsigned long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        fprintf(stderr, "ioctlsocket failed: %d\n", WSAGetLastError());
        return -1;
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) { perror("fcntl"); return -1; }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) { perror("fcntl"); return -1; }
#endif
    return 0;
}

int send_msg(int fd, const struct sockaddr_in *addr, uint16_t type, uint32_t seq,
             const void *body, uint32_t body_len) {
    struct msg_header hdr;
    hdr.magic = htons(PROTO_MAGIC);
    hdr.type = htons(type);
    hdr.seq = htonl(seq);
    hdr.body_len = htonl(body_len);

    uint32_t total = sizeof(hdr) + body_len;
    char buf[MAX_MSG_SIZE];
    if (total > sizeof(buf)) return -1;

    memcpy(buf, &hdr, sizeof(hdr));
    if (body && body_len > 0)
        memcpy(buf + sizeof(hdr), body, body_len);

    ssize_t sent = sendto(fd, buf, total, 0,
                          (const struct sockaddr*)addr, sizeof(*addr));

    if (sent < 0) {
        fprintf(stderr, "sendto failed: %s\n", sock_strerror());
        return -1;
    }
    return 0;
}

int recv_msg(int fd, struct sockaddr_in *addr, struct msg_header *hdr,
             void *body, uint32_t *body_len) {
    char buf[MAX_MSG_SIZE];
#ifdef _WIN32
    int addrlen = sizeof(*addr);
    int flags = 0;
#else
    socklen_t addrlen = sizeof(*addr);
    int flags = 0;
#endif

    ssize_t n = recvfrom(fd, buf, sizeof(buf), flags, (struct sockaddr*)addr, &addrlen);
    if (n < 0) {
        int se = sock_errno();
        if (se == EAGAIN
#ifdef _WIN32
            || se == WSAEWOULDBLOCK
#else
            || se == EWOULDBLOCK
#endif
           ) return -2;
        fprintf(stderr, "recvfrom: %s\n", sock_strerror());
        return -1;
    }

    if ((size_t)n < sizeof(struct msg_header)) {
        fprintf(stderr, "Received packet too short: %zd\n", n);
        return -1;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    hdr->magic = ntohs(hdr->magic);
    hdr->type = ntohs(hdr->type);
    hdr->seq = ntohl(hdr->seq);
    hdr->body_len = ntohl(hdr->body_len);

    if (hdr->magic != PROTO_MAGIC) {
        fprintf(stderr, "Invalid magic: 0x%04X\n", hdr->magic);
        return -1;
    }

    if (hdr->body_len > MAX_MSG_SIZE - sizeof(struct msg_header)) {
        fprintf(stderr, "Body too large: %u\n", hdr->body_len);
        return -1;
    }

    if ((size_t)n < sizeof(struct msg_header) + hdr->body_len) {
        fprintf(stderr, "Packet truncated: expected %zu, got %zd\n",
                sizeof(struct msg_header) + hdr->body_len, n);
        return -1;
    }

    if (body && body_len) {
        memcpy(body, buf + sizeof(struct msg_header), hdr->body_len);
        *body_len = hdr->body_len;
    }

    return 0;
}

char* sock_strerror(void) {
#ifdef _WIN32
    static char errbuf[256];
    DWORD err = WSAGetLastError();
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, errbuf, sizeof(errbuf), NULL);
    return errbuf;
#else
    return strerror(errno);
#endif
}

int sock_errno(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void print_addr(const struct sockaddr_in *addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    printf("%s:%d", ip, ntohs(addr->sin_port));
}

uint32_t ip_str_to_uint32(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) return 0;
    return addr.s_addr;
}

void ip_uint32_to_str(uint32_t ip, char *buf, size_t len) {
    struct in_addr addr;
    addr.s_addr = ip;
    inet_ntop(AF_INET, &addr, buf, (socklen_t)len);
}

time_t get_timestamp(void) {
    return time(NULL);
}

void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}

int64_t get_time_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

int set_udp_buf_size(int fd, int desired, int *actual_snd, int *actual_rcv) {
    int tmp;
    socklen_t optlen;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&desired, sizeof(desired)) < 0) {
        fprintf(stderr, "setsockopt SO_SNDBUF(%d) failed: %s\n", desired, sock_strerror());
    }
    optlen = sizeof(tmp);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&tmp, &optlen) == 0) {
        *actual_snd = tmp;
    } else {
        *actual_snd = desired;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&desired, sizeof(desired)) < 0) {
        fprintf(stderr, "setsockopt SO_RCVBUF(%d) failed: %s\n", desired, sock_strerror());
    }
    optlen = sizeof(tmp);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&tmp, &optlen) == 0) {
        *actual_rcv = tmp;
    } else {
        *actual_rcv = desired;
    }

    return 0;
}
