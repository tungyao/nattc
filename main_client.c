#include "client.h"
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
    const char *server_ip = "127.0.0.1";
    uint16_t server_port = 8800;
    const char *client_id = NULL;
    const char *virtual_ip = NULL;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <client_id> <virtual_ip> [server_ip] [server_port]\n", argv[0]);
        fprintf(stderr, "  client_id:    unique identifier for this client\n");
        fprintf(stderr, "  virtual_ip:   virtual IP address (e.g. 10.0.0.1)\n");
        fprintf(stderr, "  server_ip:    signaling server IP (default: 127.0.0.1)\n");
        fprintf(stderr, "  server_port:  signaling server port (default: 8800)\n");
        return 1;
    }

    client_id = argv[1];
    virtual_ip = argv[2];

    if (argc > 3) server_ip = argv[3];
    if (argc > 4) server_port = (uint16_t)atoi(argv[4]);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    struct client_context ctx;
    if (client_init(&ctx, server_ip, server_port, client_id, virtual_ip) < 0) {
        fprintf(stderr, "Failed to initialize client\n");
        return 1;
    }

    printf("Client %s running with VIP %s\n", client_id, virtual_ip);
    client_run(&ctx);

    client_cleanup(&ctx);
    printf("Client stopped\n");
    return 0;
}