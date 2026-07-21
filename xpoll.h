#ifndef XPOLL_H
#define XPOLL_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define XPOLLIN   0x001
#define XPOLLPRI  0x002
#define XPOLLOUT  0x004
#define XPOLLERR  0x008
#define XPOLLHUP  0x010
#define XPOLLET   0x80000000

#define XPOLL_CTL_ADD 1
#define XPOLL_CTL_MOD 2
#define XPOLL_CTL_DEL 3

typedef struct xpoll_event {
    uint32_t   events;
    union {
        void     *ptr;
        int       fd;
        uint32_t  u32;
        uint64_t  u64;
    } data;
} xpoll_event;

typedef struct xpoll_s xpoll_t;

xpoll_t* xpoll_create(int size);
int      xpoll_ctl(xpoll_t *xp, int op, int fd, struct xpoll_event *ev);
int      xpoll_wait(xpoll_t *xp, struct xpoll_event *events, int maxevents, int timeout_ms);
int      xpoll_close(xpoll_t *xp);

#ifdef __cplusplus
}
#endif

#endif /* XPOLL_H */
