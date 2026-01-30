// Example: Using the WebSocket Client Library
// Compile: gcc examples/client_example.c src/wsclient.c src/websocket.c -lssl -lcrypto -lpthread -o client

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/ws_client.h"

// Global client for signal handler
ws_client_context_t* g_client = NULL;

// Signal handler for clean shutdown
void signal_handler(int sig) {
    (void)sig;
    if (g_client) {
        printf("\nShutting down...\n");
        ws_client_disconnect(g_client);
    }
}

// Callbacks
void on_open(ws_client_t* client) {
    (void)client;
    printf("Connected to server!\n");
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    (void)client;

    if (type == WS_OPCODE_TEXT) {
        printf("Received text: %.*s\n", (int)size, data);
    } else if (type == WS_OPCODE_BINARY) {
        printf("Received binary data: %zu bytes\n", size);
    }
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)client;
    printf("Connection closed: %d - %s\n", code, reason ? reason : "");
}

void on_error(ws_client_t* client, const char* error) {
    (void)client;
    fprintf(stderr, "Error: %s\n", error);
}

void on_pong(ws_client_t* client, const uint8_t* data, size_t len) {
    (void)client;
    (void)data;
    (void)len;
    printf("Pong received\n");
}

// ============================================================================
// Example 1: Simple blocking client
// ============================================================================

void example_blocking_client(void) {
    printf("=== Example 1: Blocking Client ===\n");

    ws_client_context_t ctx;
    ws_client_init(&ctx);
    g_client = &ctx;

    // Configure
    ws_client_config_t config = ws_client_default_config();
    config.host = "echo.websocket.org";
    config.port = 443;
    config.path = "/";
    config.use_ssl = true;
    config.ping_interval = 30;

    ws_client_configure(&ctx, &config);

    // Set callbacks
    ctx.ws_client.on_open = on_open;
    ctx.ws_client.on_message = on_message;
    ctx.ws_client.on_close = on_close;
    ctx.ws_client.on_error = on_error;
    ctx.ws_client.on_pong = on_pong;

    // Connect
    ws_error_t err = ws_client_connect(&ctx);
    if (err != WS_OK) {
        fprintf(stderr, "Failed to connect: %s\n", ws_strerror(err));
        ws_client_cleanup(&ctx);
        return;
    }

    // Send a message
    ws_client_send_text(&ctx, "Hello, WebSocket!");

    // Run (blocks until disconnect)
    ws_client_run(&ctx);

    // Cleanup
    ws_client_cleanup(&ctx);
    g_client = NULL;
    printf("Client shut down cleanly\n");
}

// ============================================================================
// Example 2: Non-blocking client with manual polling
// ============================================================================

void example_nonblocking_client(void) {
    printf("=== Example 2: Non-blocking Client ===\n");

    ws_client_context_t ctx;
    ws_client_init(&ctx);
    g_client = &ctx;

    // Configure for non-blocking
    ws_client_config_t config = ws_client_default_config();
    config.host = "echo.websocket.org";
    config.port = 443;
    config.path = "/";
    config.use_ssl = true;
    config.non_blocking = true;

    ws_client_configure(&ctx, &config);

    // Set callbacks
    ctx.ws_client.on_open = on_open;
    ctx.ws_client.on_message = on_message;
    ctx.ws_client.on_close = on_close;
    ctx.ws_client.on_error = on_error;

    // Connect
    ws_error_t err = ws_client_connect(&ctx);
    if (err != WS_OK) {
        fprintf(stderr, "Failed to connect: %s\n", ws_strerror(err));
        ws_client_cleanup(&ctx);
        return;
    }

    // Main loop with manual polling
    int count = 0;
    ctx.running = true;

    while (ctx.running && ws_client_is_connected(&ctx)) {
        // Poll for events (100ms timeout)
        int events = ws_client_poll(&ctx, 100);

        if (events < 0) {
            fprintf(stderr, "Poll error\n");
            break;
        }

        // Do other work here
        if (count % 50 == 0) {  // Every 5 seconds
            char msg[64];
            snprintf(msg, sizeof(msg), "Message #%d", count / 50);
            ws_client_send_text(&ctx, msg);
        }

        count++;

        // Exit after 10 messages
        if (count >= 500) {
            ws_client_disconnect(&ctx);
        }
    }

    // Cleanup
    ws_client_cleanup(&ctx);
    g_client = NULL;
    printf("Client shut down cleanly\n");
}

// ============================================================================
// Example 3: Threaded client with auto-reconnect
// ============================================================================

void example_threaded_client(void) {
    printf("=== Example 3: Threaded Client with Auto-Reconnect ===\n");

    ws_client_context_t ctx;
    ws_client_init(&ctx);
    g_client = &ctx;

    // Configure with auto-reconnect
    ws_client_config_t config = ws_client_default_config();
    config.host = "echo.websocket.org";
    config.port = 443;
    config.path = "/";
    config.use_ssl = true;
    config.auto_reconnect = true;
    config.max_reconnect_attempts = 5;
    config.reconnect_delay_ms = 2000;

    ws_client_configure(&ctx, &config);

    // Set callbacks
    ctx.ws_client.on_open = on_open;
    ctx.ws_client.on_message = on_message;
    ctx.ws_client.on_close = on_close;
    ctx.ws_client.on_error = on_error;

    // Connect
    ws_error_t err = ws_client_connect(&ctx);
    if (err != WS_OK) {
        fprintf(stderr, "Failed to connect: %s\n", ws_strerror(err));
        ws_client_cleanup(&ctx);
        return;
    }

    // Start in background thread
    err = ws_client_start_thread(&ctx);
    if (err != WS_OK) {
        fprintf(stderr, "Failed to start thread: %s\n", ws_strerror(err));
        ws_client_cleanup(&ctx);
        return;
    }

    // Main thread can do other work
    for (int i = 0; i < 10; i++) {
        sleep(2);

        if (ws_client_is_connected(&ctx)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Background message #%d", i);
            ws_client_send_text(&ctx, msg);
        }

        // Print stats
        ws_statistics_t stats;
        ws_client_get_stats(&ctx, &stats);
        printf("Stats: %lu bytes sent, %lu bytes received, %lu reconnects\n", (unsigned long)stats.bytes_sent,
               (unsigned long)stats.bytes_received, (unsigned long)ws_client_get_reconnect_count(&ctx));
    }

    // Stop thread and cleanup
    ws_client_stop_thread(&ctx);
    ws_client_cleanup(&ctx);
    g_client = NULL;
    printf("Client shut down cleanly\n");
}

// ============================================================================
// Example 4: Interactive chat client
// ============================================================================

void example_interactive_client(void) {
    printf("=== Example 4: Interactive Chat Client ===\n");
    printf("Type messages and press Enter. Type 'quit' to exit.\n\n");

    ws_client_context_t ctx;
    ws_client_init(&ctx);
    g_client = &ctx;

    // Configure
    ws_client_config_t config = ws_client_default_config();
    config.host = "echo.websocket.org";
    config.port = 443;
    config.path = "/";
    config.use_ssl = true;

    ws_client_configure(&ctx, &config);

    // Set callbacks
    ctx.ws_client.on_open = on_open;
    ctx.ws_client.on_message = on_message;
    ctx.ws_client.on_close = on_close;
    ctx.ws_client.on_error = on_error;

    // Connect
    ws_error_t err = ws_client_connect(&ctx);
    if (err != WS_OK) {
        fprintf(stderr, "Failed to connect: %s\n", ws_strerror(err));
        ws_client_cleanup(&ctx);
        return;
    }

    // Start in background thread
    err = ws_client_start_thread(&ctx);
    if (err != WS_OK) {
        fprintf(stderr, "Failed to start thread: %s\n", ws_strerror(err));
        ws_client_cleanup(&ctx);
        return;
    }

    // Interactive input loop
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        if (strcmp(line, "quit") == 0) {
            break;
        }

        if (strlen(line) > 0 && ws_client_is_connected(&ctx)) {
            ws_client_send_text(&ctx, line);
        }
    }

    // Cleanup
    ws_client_stop_thread(&ctx);
    ws_client_cleanup(&ctx);
    g_client = NULL;
    printf("\nGoodbye!\n");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    if (argc > 1) {
        if (strcmp(argv[1], "1") == 0) {
            example_blocking_client();
        } else if (strcmp(argv[1], "2") == 0) {
            example_nonblocking_client();
        } else if (strcmp(argv[1], "3") == 0) {
            example_threaded_client();
        } else if (strcmp(argv[1], "4") == 0) {
            example_interactive_client();
        } else {
            printf("Usage: %s [1|2|3|4]\n", argv[0]);
            printf("  1 - Blocking client\n");
            printf("  2 - Non-blocking client\n");
            printf("  3 - Threaded client with auto-reconnect\n");
            printf("  4 - Interactive chat client\n");
            return 1;
        }
    } else {
        // Default: run interactive client
        example_interactive_client();
    }

    // Cleanup OpenSSL
    EVP_cleanup();
    ERR_free_strings();

    return 0;
}
