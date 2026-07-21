#ifndef IOCP_H
#define IOCP_H

#ifdef _WIN32

#include <stdint.h>
#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOCP_IN   0x001
#define IOCP_PRI  0x002
#define IOCP_OUT  0x004
#define IOCP_ERR  0x008
#define IOCP_HUP  0x010
#define IOCP_ET   0x80000000

#define IOCP_CTL_ADD 1
#define IOCP_CTL_MOD 2
#define IOCP_CTL_DEL 3

typedef struct __attribute__((__packed__)) iocp_event {
    uint32_t   events;
    union {
        void     *ptr;
        int       fd;
        uint32_t  u32;
        uint64_t  u64;
    } data;
} iocp_event;

typedef struct iocp_s iocp_t;

iocp_t* iocp_create(int size);
int     iocp_ctl(iocp_t *iocp, int op, SOCKET sock, struct iocp_event *event);
int     iocp_wait(iocp_t *iocp, struct iocp_event *events, int maxevents, int timeout_ms);
int     iocp_close(iocp_t *iocp);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* IOCP_H */
