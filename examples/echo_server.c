#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/ws_server.h"

static ws_server_t* server = NULL;

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    if (type == WS_OPCODE_TEXT) {
        ws_send_text_len(client, (const char*)data, size);
    } else if (type == WS_OPCODE_BINARY) {
        ws_send_binary(client, data, size);
    }
}

void handle_sigint(int sig) {
    (void)sig;
    if (server) ws_server_stop(server);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    signal(SIGINT, handle_sigint);

    ws_server_config_t config = {
        .port = 9001,
        .thread_count = 1,
        .on_message = on_message,
        .timeout_ms = 1000,
    };
    server = ws_server_create(&config);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("Echo server running on port %d\n", config.port);
    ws_server_start(server);
    ws_server_destroy(server);
    return 0;
}
