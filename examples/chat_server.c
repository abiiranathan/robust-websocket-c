#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/ws_server.h"

#define MAX_NAME_LEN    32
#define MAX_CHANNEL_LEN 32
#define MAX_MESSAGE_LEN 512

typedef struct {
    char name[MAX_NAME_LEN];
    char channel[MAX_CHANNEL_LEN];
} user_ctx_t;

ws_server_t* chat_server = NULL;

typedef struct {
    const char* channel;
    ws_client_t* sender;
    bool exclude_sender;
} broadcast_args_t;

// Filter function for broadcast
bool channel_filter(ws_client_t* client, void* arg) {
    broadcast_args_t* args = (broadcast_args_t*)arg;
    user_ctx_t* ctx = (user_ctx_t*)ws_get_user_data(client);

    // Check if client has been initialized
    if (!ctx) return false;

    // Check if client is in the same channel
    if (strcmp(ctx->channel, args->channel) != 0) {
        return false;
    }

    // Check if we should exclude sender
    if (args->exclude_sender && client == args->sender) {
        return false;
    }

    return true;
}

void send_to_channel(ws_client_t* sender, const char* channel, const char* msg, bool exclude_sender) {
    char json[1024];
    user_ctx_t* sender_ctx = (user_ctx_t*)ws_get_user_data(sender);

    if (!sender_ctx) return;

    snprintf(json, sizeof(json), "{\"type\":\"message\",\"user\":\"%s\",\"channel\":\"%s\",\"message\":\"%s\"}",
             sender_ctx->name, channel, msg);

    broadcast_args_t args = {.channel = channel, .sender = sender, .exclude_sender = exclude_sender};

    ws_server_broadcast_text_filter(chat_server, json, channel_filter, &args);
}

void send_system_message(ws_client_t* client, const char* message) {
    char json[512];
    snprintf(json, sizeof(json), "{\"type\":\"system\",\"message\":\"%s\"}", message);
    ws_send_text(client, json);
}

void send_error_message(ws_client_t* client, const char* error) {
    char json[512];
    snprintf(json, sizeof(json), "{\"type\":\"error\",\"message\":\"%s\"}", error);
    ws_send_text(client, json);
}

void process_command(ws_client_t* client, char* text) {
    user_ctx_t* ctx = (user_ctx_t*)ws_get_user_data(client);
    if (!ctx) return;

    // Remove trailing newlines
    text[strcspn(text, "\r\n")] = 0;

    if (text[0] == '/') {
        char* cmd = strtok(text, " ");
        char* arg = strtok(NULL, "");

        if (strcmp(cmd, "/nick") == 0 && arg) {
            // Validate nickname length
            if (strlen(arg) > MAX_NAME_LEN - 1) {
                send_error_message(client, "Nickname too long");
                return;
            }
            strncpy(ctx->name, arg, MAX_NAME_LEN - 1);
            ctx->name[MAX_NAME_LEN - 1] = '\0';
            send_system_message(client, "Nickname updated");
        } else if (strcmp(cmd, "/join") == 0 && arg) {
            // Validate channel name length
            if (strlen(arg) > MAX_CHANNEL_LEN - 1) {
                send_error_message(client, "Channel name too long");
                return;
            }
            char old_channel[MAX_CHANNEL_LEN];
            strcpy(old_channel, ctx->channel);
            strncpy(ctx->channel, arg, MAX_CHANNEL_LEN - 1);
            ctx->channel[MAX_CHANNEL_LEN - 1] = '\0';

            char msg[256];
            snprintf(msg, sizeof(msg), "Switched to channel: %s", ctx->channel);
            send_system_message(client, msg);
        } else if (strcmp(cmd, "/help") == 0) {
            send_system_message(client, "Commands: /nick <name>, /join <channel>, /help");
        } else {
            send_error_message(client, "Unknown command. Try /help");
        }
    } else {
        // Regular message - send to channel
        send_to_channel(client, ctx->channel, text, false);
    }
}

// Callbacks
void on_open(ws_client_t* client) {
    user_ctx_t* ctx = malloc(sizeof(user_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate user context\n");
        ws_close(client, WS_STATUS_ABNORMAL, "Server error");
        return;
    }

    snprintf(ctx->name, MAX_NAME_LEN, "User%d", client->socket_fd);
    strncpy(ctx->channel, "general", MAX_CHANNEL_LEN - 1);
    ctx->channel[MAX_CHANNEL_LEN - 1] = '\0';

    ws_set_user_data(client, ctx);

    printf("[%d] Client connected\n", client->socket_fd);
    send_system_message(client, "Welcome! Commands: /nick <name>, /join <channel>, /help");
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    if (type != WS_OPCODE_TEXT) {
        return;
    }

    // Limit message size
    if (size > MAX_MESSAGE_LEN) {
        send_error_message(client, "Message too long");
        return;
    }

    char* text = malloc(size + 1);
    if (!text) {
        send_error_message(client, "Server error");
        return;
    }

    memcpy(text, data, size);
    text[size] = '\0';

    process_command(client, text);
    free(text);
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)code;
    (void)reason;

    printf("[%d] Client disconnected\n", client->socket_fd);

    user_ctx_t* ctx = (user_ctx_t*)ws_get_user_data(client);
    if (ctx) {
        free(ctx);
    }
}

void handle_sigint(int sig) {
    (void)sig;
    printf("\n[Server] Shutting down...\n");
    if (chat_server) {
        ws_server_stop(chat_server);
    }
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);

    signal(SIGINT, handle_sigint);

    ws_server_config_t config = {
        .port = 8081,
        .thread_count = 0,  // Auto-detect CPU count
        .timeout_ms = 100,
        .on_open = on_open,
        .on_message = on_message,
        .on_close = on_close,
    };

    int arg_idx = 1;

    // Parse arguments: [port] [cert] [key]
    if (argc > arg_idx && isdigit(argv[arg_idx][0])) {
        config.port = atoi(argv[arg_idx]);
        if (config.port <= 0 || config.port > 65535) {
            fprintf(stderr, "Invalid port number\n");
            return 1;
        }
        arg_idx++;
    }

    // SSL/TLS configuration
    if (argc > arg_idx + 1) {
        config.ssl_cert = argv[arg_idx];
        config.ssl_key = argv[arg_idx + 1];
        printf("[Server] TLS enabled: cert=%s, key=%s\n", config.ssl_cert, config.ssl_key);
    }

    chat_server = ws_server_create(&config);
    if (!chat_server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("[Server] Starting on port %d (TLS: %s)...\n", config.port, config.ssl_cert ? "enabled" : "disabled");

    ws_server_start(chat_server);

    printf("[Server] Cleaning up...\n");
    ws_server_destroy(chat_server);

    return 0;
}
