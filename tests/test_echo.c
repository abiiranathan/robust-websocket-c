#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "../include/websocket.h"

int passed = 0;
int failed = 0;
int connection_complete = 0;

// ============================
// Callback functions for WebSocket client
// ============================

void on_open(ws_client_t* client) {
    (void)client;
    printf("Connected to server\n");
    ws_send_text(client, "Hello World", 11);
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    if (type == WS_OPCODE_TEXT) {
        if (size == 0) {
            printf("Received Empty Text Message\n");
            passed++;
            // Final check passed
            ws_close(client, 1000, "Normal Closure");
            return;
        }

        char* text = malloc(size + 1);
        if (!text) {
            printf("Memory allocation failed\n");
            failed++;
            return;
        }
        memcpy(text, data, size);
        text[size] = '\0';
        printf("Received Text: %s\n", text);

        if (strcmp(text, "Hello World") == 0) {
            printf("Text Echo Passed\n");
            passed++;

            // Send Binary
            uint8_t bin[] = {0xDE, 0xAD, 0xBE, 0xEF};
            ws_send_binary(client, bin, sizeof(bin));
        } else {
            printf("Text Echo Failed: Expected 'Hello World', got '%s'\n", text);
            failed++;
            ws_close(client, 1000, "Failed");
        }
        free(text);
    } else if (type == WS_OPCODE_BINARY) {
        printf("Received Binary: %zu bytes\n", size);
        if (size == 4 && data[0] == 0xDE && data[1] == 0xAD && data[2] == 0xBE && data[3] == 0xEF) {
            printf("Binary Echo Passed\n");
            passed++;
            
            // Send Empty Text
            ws_send_text(client, NULL, 0);
        } else {
            printf("Binary Echo Failed\n");
            failed++;
            ws_close(client, 1000, "Failed");
        }
    }
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)client;
    (void)code;
    (void)reason;
    printf("Connection closed\n");
    connection_complete = 1;
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    printf("Error: %s\n", error);
    failed++;
    connection_complete = 1;
}

// ============================
// Main function
// ============================

int main() {
    setbuf(stdout, NULL);

    ws_client_t client;
    ws_init(&client);

    client.on_open = on_open;
    client.on_message = on_message;
    client.on_close = on_close;
    client.on_error = on_error;

    printf("Connecting to ws://localhost:9001 ...\n");
    ws_error_t err = ws_connect(&client, "localhost", 9001, "/");
    if (err != WS_OK) {
        fprintf(stderr, "Connect failed: %s\n", ws_strerror(err));
        ws_cleanup(&client);
        return 1;
    }

    // Run client event loop with timeout
    time_t start_time = time(NULL);
    int timeout_seconds = 5;
    uint8_t buffer[4096];

    while (!connection_complete && (time(NULL) - start_time) < timeout_seconds) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client.socket_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;  // 500ms timeout per select

        int ret = select(client.socket_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno != EINTR) {
                fprintf(stderr, "Select error: %s\n", strerror(errno));
                failed++;
                break;
            }
            continue;
        }

        // Handle data from server
        if (ret > 0 && FD_ISSET(client.socket_fd, &readfds)) {
            ssize_t n = ws_read(&client, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno != EINTR && errno != EAGAIN) {
                    fprintf(stderr, "Read error\n");
                    failed++;
                    break;
                }
            } else if (n == 0) {
                // Connection closed
                break;
            } else {
                err = ws_consume(&client, buffer, (size_t)n);
                if (err != WS_OK) {
                    fprintf(stderr, "Consume error: %s\n", ws_strerror(err));
                    failed++;
                    break;
                }
            }
        }
    }

    if (!connection_complete) {
        printf("Test timeout: Connection did not complete within %d seconds\n", timeout_seconds);
        failed++;
    }

    ws_cleanup(&client);

    if (failed > 0) {
        printf("Test Failed: %d errors occurred\n", failed);
        return 1;
    }
    if (passed < 3) {
        printf("Test Failed: Not all tests passed (passed: %d)\n", passed);
        return 1;
    }

    printf("All tests passed\n");
    return 0;
}
