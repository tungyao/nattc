#include "iocp.h"

#ifdef _WIN32

#ifndef WSA_MAXIMUM_WAIT_EVENTS
#define WSA_MAXIMUM_WAIT_EVENTS 64
#endif

typedef struct iocp_sock_s {
    SOCKET              sock;
    uint32_t            events;
    struct iocp_event   user;
    WSAEVENT            wevent;
    struct iocp_sock_s *next;
} iocp_sock_t;

struct iocp_s {
    iocp_sock_t     *socks;
    int              count;
    CRITICAL_SECTION lock;
};

iocp_t* iocp_create(int size) {
    iocp_t *iocp;
    if (size <= 0) { WSASetLastError(WSAEINVAL); return NULL; }
    iocp = (iocp_t*)malloc(sizeof(iocp_t));
    if (!iocp) { WSASetLastError(WSA_NOT_ENOUGH_MEMORY); return NULL; }
    memset(iocp, 0, sizeof(*iocp));
    InitializeCriticalSection(&iocp->lock);
    return iocp;
}

int iocp_ctl(iocp_t *iocp, int op, SOCKET sock, struct iocp_event *event) {
    iocp_sock_t *prev, *curr;
    long net_events;

    if (!iocp || sock == INVALID_SOCKET) { WSASetLastError(WSAEINVAL); return -1; }

    EnterCriticalSection(&iocp->lock);

    prev = NULL;
    curr = iocp->socks;
    while (curr) {
        if (curr->sock == sock) break;
        prev = curr;
        curr = curr->next;
    }

    switch (op) {
    case IOCP_CTL_ADD:
        if (curr) {
            curr->events = event->events;
            curr->user   = *event;
            net_events = FD_CLOSE;
            if (event->events & (IOCP_IN | IOCP_PRI)) net_events |= FD_READ | FD_ACCEPT;
            if (event->events & IOCP_OUT)             net_events |= FD_WRITE | FD_CONNECT;
            WSAEventSelect(sock, curr->wevent, net_events);
            LeaveCriticalSection(&iocp->lock);
            return 0;
        }
        if (iocp->count >= WSA_MAXIMUM_WAIT_EVENTS) {
            LeaveCriticalSection(&iocp->lock);
            WSASetLastError(WSAENOBUFS);
            return -1;
        }
        curr = (iocp_sock_t*)malloc(sizeof(iocp_sock_t));
        if (!curr) {
            LeaveCriticalSection(&iocp->lock);
            WSASetLastError(WSA_NOT_ENOUGH_MEMORY);
            return -1;
        }
        memset(curr, 0, sizeof(*curr));
        curr->sock   = sock;
        curr->events = event->events;
        curr->user   = *event;
        curr->wevent = WSACreateEvent();
        if (curr->wevent == WSA_INVALID_EVENT) {
            free(curr);
            LeaveCriticalSection(&iocp->lock);
            return -1;
        }
        net_events = FD_CLOSE;
        if (event->events & (IOCP_IN | IOCP_PRI)) net_events |= FD_READ | FD_ACCEPT;
        if (event->events & IOCP_OUT)             net_events |= FD_WRITE | FD_CONNECT;
        if (WSAEventSelect(sock, curr->wevent, net_events) != 0) {
            WSACloseEvent(curr->wevent);
            free(curr);
            LeaveCriticalSection(&iocp->lock);
            return -1;
        }
        curr->next  = iocp->socks;
        iocp->socks = curr;
        iocp->count++;
        break;

    case IOCP_CTL_MOD:
        if (!curr) {
            LeaveCriticalSection(&iocp->lock);
            WSASetLastError(WSAENOTSOCK);
            return -1;
        }
        curr->events = event->events;
        curr->user   = *event;
        net_events = FD_CLOSE;
        if (event->events & (IOCP_IN | IOCP_PRI)) net_events |= FD_READ | FD_ACCEPT;
        if (event->events & IOCP_OUT)             net_events |= FD_WRITE | FD_CONNECT;
        WSAEventSelect(sock, curr->wevent, net_events);
        break;

    case IOCP_CTL_DEL:
        if (!curr) {
            LeaveCriticalSection(&iocp->lock);
            return 0;
        }
        WSAEventSelect(sock, NULL, 0);
        WSACloseEvent(curr->wevent);
        if (prev) prev->next = curr->next;
        else      iocp->socks = curr->next;
        free(curr);
        iocp->count--;
        break;

    default:
        LeaveCriticalSection(&iocp->lock);
        WSASetLastError(WSAEINVAL);
        return -1;
    }

    LeaveCriticalSection(&iocp->lock);
    return 0;
}

int iocp_wait(iocp_t *iocp, struct iocp_event *events, int maxevents, int timeout_ms) {
    int            total, idx, ready;
    DWORD          wait_ms, start, res, elapsed;
    WSAEVENT       wsa_arr[WSA_MAXIMUM_WAIT_EVENTS];
    iocp_sock_t   *refs[WSA_MAXIMUM_WAIT_EVENTS];

    if (!iocp || !events || maxevents <= 0) { WSASetLastError(WSAEINVAL); return -1; }

    EnterCriticalSection(&iocp->lock);
    total = iocp->count;
    if (total == 0) {
        LeaveCriticalSection(&iocp->lock);
        if (timeout_ms != 0)
            Sleep((timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms);
        return 0;
    }
    if (total > WSA_MAXIMUM_WAIT_EVENTS) total = WSA_MAXIMUM_WAIT_EVENTS;

    {
        iocp_sock_t *s = iocp->socks;
        idx = 0;
        while (s && idx < total) {
            wsa_arr[idx] = s->wevent;
            refs[idx]    = s;
            s = s->next;
            idx++;
        }
        total = idx;
    }
    LeaveCriticalSection(&iocp->lock);

    wait_ms = (timeout_ms < 0) ? WSA_INFINITE : (DWORD)timeout_ms;
    start   = (timeout_ms > 0) ? GetTickCount() : 0;
    ready   = 0;

    for (;;) {
        if (ready > 0) {
            wait_ms = 0;
        } else if (timeout_ms > 0 && start > 0) {
            elapsed = GetTickCount() - start;
            if (elapsed >= (DWORD)timeout_ms) break;
            wait_ms = (DWORD)timeout_ms - elapsed;
        }

        res = WSAWaitForMultipleEvents(total, wsa_arr, FALSE, wait_ms, FALSE);
        if (res == WSA_WAIT_TIMEOUT || res == WSA_WAIT_IO_COMPLETION) break;
        if (res < WSA_WAIT_EVENT_0 || res >= WSA_WAIT_EVENT_0 + (DWORD)total) return -1;

        idx = res - WSA_WAIT_EVENT_0;
        {
            WSANETWORKEVENTS net;
            uint32_t out, result;
            if (WSAEnumNetworkEvents(refs[idx]->sock, wsa_arr[idx], &net) != 0) continue;

            out = 0;
            if (net.lNetworkEvents & (FD_READ | FD_ACCEPT)) out |= IOCP_IN;
            if (net.lNetworkEvents & FD_WRITE)              out |= IOCP_OUT;
            if (net.lNetworkEvents & FD_CONNECT)            out |= IOCP_OUT;
            if (net.lNetworkEvents & FD_CLOSE)              out |= IOCP_HUP;
            if (net.iErrorCode[FD_READ_BIT] != 0 ||
                net.iErrorCode[FD_CLOSE_BIT] != 0)         out |= IOCP_ERR;

            result = out & (refs[idx]->events | IOCP_ERR | IOCP_HUP);
            if (result) {
                events[ready]       = refs[idx]->user;
                events[ready].events = result;
                ready++;
                if (ready >= maxevents) break;
            }
        }
    }

    return ready;
}

int iocp_close(iocp_t *iocp) {
    iocp_sock_t *s, *next;
    if (!iocp) return 0;

    EnterCriticalSection(&iocp->lock);
    s = iocp->socks;
    while (s) {
        next = s->next;
        WSAEventSelect(s->sock, NULL, 0);
        WSACloseEvent(s->wevent);
        free(s);
        s = next;
    }
    iocp->socks = NULL;
    iocp->count = 0;
    LeaveCriticalSection(&iocp->lock);

    DeleteCriticalSection(&iocp->lock);
    free(iocp);
    return 0;
}

#endif /* _WIN32 */
