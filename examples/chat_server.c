#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../include/websocket.h"

#define MAX_EVENTS       64
#define READ_BUFFER_SIZE 4096
#define MAX_NAME_LEN     32

typedef struct {
    char name[MAX_NAME_LEN];
    char channel[MAX_NAME_LEN];
} user_ctx_t;

typedef struct client_node {
    ws_client_t* client;
    struct client_node* next;
} client_node_t;

client_node_t* client_list = NULL;

void add_client(ws_client_t* client) {
    client_node_t* node = malloc(sizeof(client_node_t));
    node->client = client;
    node->next = client_list;
    client_list = node;
}

void remove_client(ws_client_t* client) {
    client_node_t** curr = &client_list;
    while (*curr) {
        if ((*curr)->client == client) {
            client_node_t* temp = *curr;
            *curr = (*curr)->next;
            if (client->user_data) free(client->user_data);
            free(temp);
            return;
        }
        curr = &(*curr)->next;
    }
}

void send_to_channel(ws_client_t* sender, const char* channel, const char* msg, bool exclude_sender) {
    char json[1024];
    user_ctx_t* sender_ctx = (user_ctx_t*)sender->user_data;

    // Simple JSON construction (escaping omitted for brevity)
    snprintf(json, sizeof(json), "{\"user\": \"%s\", \"channel\": \"%s\", \"message\": \"%s\"}", sender_ctx->name,
             channel, msg);

    client_node_t* curr = client_list;
    while (curr) {
        user_ctx_t* ctx = (user_ctx_t*)curr->client->user_data;
        if (curr->client->state == WS_STATE_OPEN && strcmp(ctx->channel, channel) == 0) {
            if (!exclude_sender || curr->client != sender) {
                ws_send_text(curr->client, json);
            }
        }
        curr = curr->next;
    }
}

void process_command(ws_client_t* client, char* text) {
    user_ctx_t* ctx = (user_ctx_t*)client->user_data;

    // Remove newline
    text[strcspn(text, "\r\n")] = 0;

    if (text[0] == '/') {
        char* cmd = strtok(text, " ");
        char* arg = strtok(NULL, "");

        if (strcmp(cmd, "/nick") == 0 && arg) {
            // Change nickname
            strncpy(ctx->name, arg, MAX_NAME_LEN - 1);
            ws_send_text(client, "{\"type\": \"system\", \"message\": \"Nickname changed\"}");
        } else if (strcmp(cmd, "/join") == 0 && arg) {
            // Join channel
            strncpy(ctx->channel, arg, MAX_NAME_LEN - 1);
            ws_send_text(client, "{\"type\": \"system\", \"message\": \"Joined channel\"}");
        } else {
            ws_send_text(client, "{\"type\": \"error\", \"message\": \"Unknown command\"}");
        }

    } else {
        // Chat message
        send_to_channel(client, ctx->channel, text, false);
    }
}

// Callbacks
void on_open(ws_client_t* client) {
    user_ctx_t* ctx = malloc(sizeof(user_ctx_t));
    snprintf(ctx->name, MAX_NAME_LEN, "User%d", client->socket_fd);
    strcpy(ctx->channel, "general");
    client->user_data = ctx;

    printf("Client connected (fd=%d)\n", client->socket_fd);

    ws_send_text(client, "{\"type\": \"welcome\", \"message\": \"Welcome! Commands: /nick <name>, /join <channel>\"}");
}

void on_message(ws_client_t* client, const uint8_t* data, size_t size, int type) {
    if (type == WS_OPCODE_TEXT) {
        char* text = malloc(size + 1);
        memcpy(text, data, size);
        text[size] = '\0';

        process_command(client, text);
        free(text);
    }
}

void on_close(ws_client_t* client, int code, const char* reason) {
    (void)code;
    (void)reason;
    printf("Client disconnected (fd=%d)\n", client->socket_fd);
}

void on_error(ws_client_t* client, const char* error) {
    fprintf(stderr, "Client error (fd=%d): %s\n", client->socket_fd, error);
}

int make_socket_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    setbuf(stdout, NULL);
    int server_fd, epoll_fd;
    struct sockaddr_in address;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    make_socket_non_blocking(server_fd);

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8081);  // Port 8081 for chat server

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    epoll_fd = epoll_create1(0);
    ws_client_t server_dummy = {0};
    server_dummy.socket_fd = server_fd;

    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &server_dummy};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    printf("Chat server running on port 8081\n");

    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[READ_BUFFER_SIZE];

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            ws_client_t* client = (ws_client_t*)events[i].data.ptr;

            if (client == &server_dummy) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd == -1) continue;

                make_socket_non_blocking(client_fd);

                ws_client_t* new_client = malloc(sizeof(ws_client_t));
                ws_init(new_client);
                new_client->on_open = on_open;
                new_client->on_message = on_message;
                new_client->on_close = on_close;
                new_client->on_error = on_error;

                ws_accept(new_client, client_fd);
                add_client(new_client);

                struct epoll_event client_ev = {.events = EPOLLIN | EPOLLET, .data.ptr = new_client};
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
            } else {
                ssize_t bytes_read = read(client->socket_fd, buffer, sizeof(buffer));
                if (bytes_read <= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->socket_fd, NULL);
                    remove_client(client);
                    ws_cleanup(client);
                    free(client);
                } else {
                    ws_consume(client, buffer, (size_t)bytes_read);
                }
            }
        }
    }
    return 0;
}
