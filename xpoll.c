#include "xpoll.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include "iocp.h"

struct xpoll_s {
    iocp_t *iocp;
};

xpoll_t* xpoll_create(int size) {
    xpoll_t *xp = (xpoll_t*)malloc(sizeof(xpoll_t));
    if (!xp) { WSASetLastError(WSA_NOT_ENOUGH_MEMORY); return NULL; }
    xp->iocp = iocp_create(size);
    if (!xp->iocp) { free(xp); return NULL; }
    return xp;
}

int xpoll_ctl(xpoll_t *xp, int op, int fd, struct xpoll_event *ev) {
    if (!xp) { WSASetLastError(WSAEINVAL); return -1; }
    return iocp_ctl(xp->iocp, op, (SOCKET)fd, (struct iocp_event*)(void*)ev);
}

int xpoll_wait(xpoll_t *xp, struct xpoll_event *events, int maxevents, int timeout_ms) {
    if (!xp) { WSASetLastError(WSAEINVAL); return -1; }
    return iocp_wait(xp->iocp, (struct iocp_event*)(void*)events, maxevents, timeout_ms);
}

int xpoll_close(xpoll_t *xp) {
    if (!xp) return 0;
    iocp_close(xp->iocp);
    free(xp);
    return 0;
}

#else /* !_WIN32: Linux epoll */

#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

#define XPOLL_MAX_EVENTS 64

struct xpoll_s {
    int epfd;
};

xpoll_t* xpoll_create(int size) {
    (void)size;
    xpoll_t *xp = (xpoll_t*)malloc(sizeof(xpoll_t));
    if (!xp) { errno = ENOMEM; return NULL; }
    xp->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (xp->epfd < 0) { free(xp); return NULL; }
    return xp;
}

int xpoll_ctl(xpoll_t *xp, int op, int fd, struct xpoll_event *ev) {
    if (!xp) { errno = EINVAL; return -1; }
    struct epoll_event eev;
    eev.events = ev->events;
    eev.data.fd = ev->data.fd;
    return epoll_ctl(xp->epfd, op, fd, &eev);
}

int xpoll_wait(xpoll_t *xp, struct xpoll_event *events, int maxevents, int timeout_ms) {
    if (!xp) { errno = EINVAL; return -1; }
    struct epoll_event eev[XPOLL_MAX_EVENTS];
    if (maxevents > XPOLL_MAX_EVENTS) maxevents = XPOLL_MAX_EVENTS;
    int n = epoll_wait(xp->epfd, eev, maxevents, timeout_ms);
    for (int i = 0; i < n; i++) {
        events[i].events = eev[i].events;
        events[i].data.fd = eev[i].data.fd;
    }
    return n;
}

int xpoll_close(xpoll_t *xp) {
    if (!xp) return 0;
    close(xp->epfd);
    free(xp);
    return 0;
}

#endif
