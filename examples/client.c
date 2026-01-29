#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/websocket.h"

void on_open(ws_client_t* client) {
    (void)client;
    printf("Connected to server!\n");
    ws_send_text(client, "Hello from robust client!");
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    (void)client;
    if (type == WS_OPCODE_TEXT) {
        printf("Received: %.*s\n", (int)size, data);
    }
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)client;
    (void)code;
    printf("Connection closed: %s\n", reason);
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    fprintf(stderr, "Error: %s\n", error);
}

int main() {
    setbuf(stdout, NULL);
    ws_client_t client;
    ws_init(&client);

    client.on_open = on_open;
    client.on_message = on_message;
    client.on_close = on_close;
    client.on_error = on_error;

    if (ws_connect(&client, "localhost", 8080, "/") != 0) {
        return EXIT_FAILURE;
    }

    // Simple read loop
    uint8_t buffer[4096];
    while (client.state != WS_STATE_CLOSED) {
        ssize_t n = read(client.socket_fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        ws_consume(&client, buffer, (size_t)n);
    }

    ws_cleanup(&client);
    return EXIT_SUCCESS;
}
