#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/websocket.h"

void on_open(ws_client_t* client) {
    (void)client;
    printf("\n[Connected to Chat Server]\n");
    printf("Commands: /nick <name>, /join <channel>\n");
    printf("> ");
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
    printf("\n[Disconnected: %s]\n", reason ? reason : "unknown");
    exit(0);
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    fprintf(stderr, "\n[Error] %s\n> ", error);
    fflush(stderr);
}

// Callback for each line of input from stdin
// The ws_run_interactive function handles all select() logic
void handle_stdin_line(ws_client_t* client, const char* line, void* user_data) {
    (void)user_data;  // Not used in this example

    if (!client || !line || strlen(line) == 0) {
        return;
    }

    // Send the line to the server
    if (ws_send_text(client, line) != WS_OK) {
        printf("[Error] Failed to send message\n> ");
    } else {
        printf("> ");
    }
    fflush(stdout);
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
    bool use_tls = false;

    // Parse connection string
    if (argc > 1) {
        const char* uri = argv[1];

        if (strncmp(uri, "wss://", 6) == 0) {
            host = uri + 6;
            port = 443;
            use_tls = true;
        } else if (strncmp(uri, "ws://", 5) == 0) {
            host = uri + 5;
            port = 80;
        } else {
            host = uri;
            // Check for port in host:port format
            char* colon = strchr((char*)host, ':');
            if (colon) {
                *colon = '\0';
                port = atoi(colon + 1);
            }
        }
    }

    // Optional: override port
    if (argc > 2) {
        port = atoi(argv[2]);
    }

    // Enable SSL if port 443
    if (port == 443) {
        use_tls = true;
    }

    ws_set_ssl(&client, use_tls);

    printf("Connecting to %s:%d (TLS: %s)...\n", host, port, use_tls ? "on" : "off");

    ws_error_t err = ws_connect(&client, host, port, "/");
    if (err != WS_OK) {
        fprintf(stderr, "Failed to connect: %s\n", ws_strerror(err));
        ws_cleanup(&client);
        return EXIT_FAILURE;
    }

    // Run interactive loop - handles stdin and socket communication
    // No need to deal with select() calls or buffer management!
    err = ws_run_interactive(&client, handle_stdin_line, NULL, 1000);
    if (err != WS_OK) {
        fprintf(stderr, "Interactive session error: %s\n", ws_strerror(err));
    }

    ws_cleanup(&client);
    return EXIT_SUCCESS;
}
