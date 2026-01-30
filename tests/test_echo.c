#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/websocket.h"

int passed = 0;
int failed = 0;

void on_open(ws_client_t* client) {
    printf("Connected to server\n");
    ws_send_text(client, "Hello World");
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    if (type == WS_OPCODE_TEXT) {
        char* text = malloc(size + 1);
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
            ws_close(client, 1000, "Normal Closure");
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
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    printf("Error: %s\n", error);
    failed++;
}

int main() {
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
        return 1;
    }

    uint8_t buffer[4096];
    while (client.state != WS_STATE_CLOSED) {
        ssize_t n = ws_read(&client, buffer, sizeof(buffer));
        if (n > 0) {
            ws_consume(&client, buffer, (size_t)n);
        } else if (n < 0) {
            fprintf(stderr, "Read error or timeout\n");
            break;
        } else {
            printf("Socket closed by peer\n");
            break;
        }
    }

    ws_cleanup(&client);

    if (failed > 0) {
        return 1;
    }
    if (passed < 2) { // Expect Text and Binary passes
        printf("Test Failed: Not all tests passed\n");
        return 1;
    }
    
    printf("All tests passed\n");
    return 0;
}
