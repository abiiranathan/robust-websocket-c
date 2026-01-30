#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "../include/websocket.h"

// Simple line buffer for non-blocking stdin reading
typedef struct {
    char buffer[4096];
    size_t len;
} input_buffer_t;

void on_open(ws_client_t* client) {
    (void)client;
    printf("\n[Connected to Chat Server]\n");
    printf("Commands: /nick <name>, /join <channel>\n> ");
    fflush(stdout);
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    (void)client;
    if (type == WS_OPCODE_TEXT) {
        printf("\r%.*s\n> ", (int)size, data);
        fflush(stdout);
    }
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)client;
    (void)code;
    printf("\n[Disconnected: %s]\n", reason);
    exit(0);
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    fprintf(stderr, "\n[Error] %s\n> ", error);
    fflush(stderr);
}

int main(int argc, char** argv) {
    // Disable stdout buffering to ensure prompts appear immediately
    setbuf(stdout, NULL);

    ws_client_t client;
    ws_init(&client);

    client.on_open = on_open;
    client.on_message = on_message;
    client.on_close = on_close;
    client.on_error = on_error;

    const char* host = "localhost";
    int port = 8081;
    bool tls = false;

    if (argc > 1) {
        // Parse URI
        char* uri = argv[1];
        if (strncmp(uri, "ws://", 5) == 0) {
            host = uri + 5;
            port = 80;
        } else if (strncmp(uri, "wss://", 6) == 0) {
            host = uri + 6;
            port = 443;
            tls = true;
        } else {
            host = uri;
        }

        // Split host and port if present
        char* p = strchr(host, ':');
        if (p) {
            *p = '\0';  // Note: modifies argv[1]
            port = atoi(p + 1);
        }
    }

    // Allow manual port override
    if (argc > 2) port = atoi(argv[2]);

    ws_set_ssl(&client, tls);

    printf("Connecting to %s:%d (TLS: %s)...\n", host, port, tls ? "on" : "off");
    if (ws_connect(&client, host, port, "/") != WS_OK) {
        fprintf(stderr, "Failed to connect to server\n");
        return EXIT_FAILURE;
    }

    uint8_t buffer[4096];
    input_buffer_t input = {0};

    int stdin_fd = STDIN_FILENO;

    // Main Event Loop
    while (client.state != WS_STATE_CLOSED) {
        fd_set fds;
        FD_ZERO(&fds);

        if (stdin_fd != -1) FD_SET(stdin_fd, &fds);
        FD_SET(client.socket_fd, &fds);

        int max_fd = (client.socket_fd > stdin_fd) ? client.socket_fd : stdin_fd;

        // Wait for activity
        if (select(max_fd + 1, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // Handle Input from User
        if (stdin_fd != -1 && FD_ISSET(stdin_fd, &fds)) {
            ssize_t n = read(stdin_fd, input.buffer + input.len, sizeof(input.buffer) - input.len - 1);
            if (n > 0) {
                input.len += (size_t)n;
                input.buffer[input.len] = 0;

                // Process lines
                char* newline;
                while ((newline = strchr(input.buffer, '\n'))) {
                    *newline = 0;

                    // Handle CR if present (e.g. from telnet style input or some terminals)
                    if (newline > input.buffer && *(newline - 1) == '\r') {
                        *(newline - 1) = 0;
                    }

                    if (strlen(input.buffer) > 0) {
                        if (ws_send_text(&client, input.buffer) != WS_OK) {
                            printf("[Error] Failed to send message (Not connected?)\n> ");
                        } else {
                            printf("> ");  // Re-print prompt
                        }
                        fflush(stdout);
                    }

                    // Move remaining data to front
                    size_t line_len = (size_t)(newline - input.buffer) + 1;
                    size_t remaining = input.len - line_len;
                    memmove(input.buffer, input.buffer + line_len, remaining);
                    input.len = remaining;
                    input.buffer[input.len] = 0;
                }

                // Buffer full protection
                if (input.len >= sizeof(input.buffer) - 1) {
                    fprintf(stderr, "Input line too long, clearing buffer\n");
                    input.len = 0;
                }
            } else {
                // EOF or Error on stdin
                stdin_fd = -1;
            }
        }

        // Handle Data from Server
        if (FD_ISSET(client.socket_fd, &fds)) {
            ssize_t n = ws_read(&client, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno != EINTR) {
                    perror("read");
                    break;
                }
            } else if (n == 0) {
                printf("\n[Server closed connection]\n");
                break;
            } else {
                ws_consume(&client, buffer, (size_t)n);
            }
        }
    }

    ws_cleanup(&client);
    return EXIT_SUCCESS;
}
