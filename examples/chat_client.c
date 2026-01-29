#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "../include/websocket.h"

// Callbacks
void on_open(ws_client_t* client) {
    (void)client;
    printf("[Connected to Chat Server]\n");
    printf("Commands: /nick <name>, /join <channel>\n");
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    (void)client;
    if (type == WS_OPCODE_TEXT) {
        printf("%.*s\n", (int)size, data);
    }
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)client;
    (void)code;
    printf("[Disconnected: %s]\n", reason);
    exit(0);
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    fprintf(stderr, "[Error] %s\n", error);
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    ws_client_t client;
    ws_init(&client);

    client.on_open = on_open;
    client.on_message = on_message;
    client.on_close = on_close;
    client.on_error = on_error;

    const char* host = "localhost";
    int port = 8081;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    printf("Connecting to %s:%d...\n", host, port);
    if (ws_connect(&client, host, port, "/") != 0) {
        return EXIT_FAILURE;
    }

    uint8_t buffer[4096];
    char input[1024];

    int stdin_fd = STDIN_FILENO;
    while (client.state != WS_STATE_CLOSED) {
        fd_set fds;
        FD_ZERO(&fds);
        if (stdin_fd != -1) FD_SET(stdin_fd, &fds);
        FD_SET(client.socket_fd, &fds);

        int max_fd = (client.socket_fd > stdin_fd) ? client.socket_fd : stdin_fd;

        if (select(max_fd + 1, &fds, NULL, NULL, NULL) < 0) {
            perror("select");
            break;
        }

        if (stdin_fd != -1 && FD_ISSET(stdin_fd, &fds)) {
            if (fgets(input, sizeof(input), stdin)) {
                // Remove newline
                input[strcspn(input, "\r\n")] = 0;
                if (strlen(input) > 0) {
                    ws_send_text(&client, input);
                }
            } else {
                stdin_fd = -1;  // Disable stdin
            }
        }

        if (FD_ISSET(client.socket_fd, &fds)) {
            ssize_t n = read(client.socket_fd, buffer, sizeof(buffer));
            if (n <= 0) break;
            ws_consume(&client, buffer, (size_t)n);
        }
    }

    ws_cleanup(&client);
    return EXIT_SUCCESS;
}
