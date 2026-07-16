#include "server.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    uint16_t port = 8800;

    if (argc > 1) {
        port = (uint16_t)atoi(argv[1]);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    struct server_context ctx;
    if (server_init(&ctx, port) < 0) {
        fprintf(stderr, "Failed to initialize server\n");
        return 1;
    }

    printf("Server listening on port %d\n", port);
    server_run(&ctx);

    server_cleanup(&ctx);
    printf("Server stopped\n");
    return 0;
}