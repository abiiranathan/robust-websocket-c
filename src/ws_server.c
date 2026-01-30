// #define _GNU_SOURCE  <-- Already defined in Makefile or websocket.h
#include "../include/ws_server.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#define MAX_EVENTS       64
#define READ_BUFFER_SIZE 4096

// --- Internal Structures ---

// Context for a single client connection within the server
typedef struct {
    ws_client_t ws_client;  // Base class
    int epoll_fd;           // The epoll instance managing this client
    struct ws_server_s* server;
    struct ws_worker_s* worker;

    // Outgoing buffer for non-blocking writes
    uint8_t* out_buf;
    size_t out_len;
    size_t out_cap;
    pthread_mutex_t out_lock;
} ws_connection_t;

// A worker thread context
typedef struct ws_worker_s {
    pthread_t thread;
    int epoll_fd;
    int pipe_fd[2];  // [0] read, [1] write - for dispatching new connections
    volatile bool running;
    ws_server_t* server;
    int id;

    // Linked list of clients handled by this worker (for broadcast/cleanup)
    pthread_mutex_t clients_lock;
    ws_connection_t** clients;
    size_t client_count;
    size_t client_cap;
} ws_worker_t;

// The main server context
struct ws_server_s {
    ws_server_config_t config;
    int listen_fd;
    volatile bool running;

    ws_worker_t* workers;
    int worker_count;

    // Global stats
    pthread_mutex_t stats_lock;
    size_t total_clients;
};

// --- Helper Functions ---

static int make_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void add_client_to_worker(ws_worker_t* worker, ws_connection_t* conn) {
    pthread_mutex_lock(&worker->clients_lock);
    if (worker->client_count >= worker->client_cap) {
        size_t new_cap = worker->client_cap == 0 ? 16 : worker->client_cap * 2;
        ws_connection_t** new_list = realloc(worker->clients, new_cap * sizeof(ws_connection_t*));
        if (new_list) {
            worker->clients = new_list;
            worker->client_cap = new_cap;
        }
    }
    if (worker->client_count < worker->client_cap) {
        worker->clients[worker->client_count++] = conn;
    }
    pthread_mutex_unlock(&worker->clients_lock);
}

static void remove_client_from_worker(ws_worker_t* worker, ws_connection_t* conn) {
    pthread_mutex_lock(&worker->clients_lock);
    for (size_t i = 0; i < worker->client_count; ++i) {
        if (worker->clients[i] == conn) {
            // Swap with last
            worker->clients[i] = worker->clients[worker->client_count - 1];
            worker->client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&worker->clients_lock);
}

// Custom Write Callback for Non-Blocking I/O
static int server_write_cb(ws_client_t* client, const uint8_t* data, size_t len) {
    ws_connection_t* conn = (ws_connection_t*)client;

    pthread_mutex_lock(&conn->out_lock);

    // Try to write directly if buffer is empty
    size_t written = 0;
    if (conn->out_len == 0) {
        ssize_t n = write(client->socket_fd, data, len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                written = 0;
            } else {
                pthread_mutex_unlock(&conn->out_lock);
                return -1;
            }
        } else {
            written = (size_t)n;
        }
    }

    // Buffer remaining
    if (written < len) {
        size_t remaining = len - written;
        if (conn->out_len + remaining > conn->out_cap) {
            size_t new_cap = conn->out_cap == 0 ? 4096 : conn->out_cap * 2;
            while (conn->out_len + remaining > new_cap) new_cap *= 2;
            uint8_t* new_buf = realloc(conn->out_buf, new_cap);
            if (!new_buf) {
                pthread_mutex_unlock(&conn->out_lock);
                return -1;
            }
            conn->out_buf = new_buf;
            conn->out_cap = new_cap;
        }
        memcpy(conn->out_buf + conn->out_len, data + written, remaining);
        conn->out_len += remaining;

        // Register for EPOLLOUT
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;  // Edge Triggered
        ev.data.ptr = conn;
        epoll_ctl(conn->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
    }

    pthread_mutex_unlock(&conn->out_lock);
    return 0;
}

// --- Worker Loop ---

static void* worker_routine(void* arg) {
    ws_worker_t* worker = (ws_worker_t*)arg;
    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[READ_BUFFER_SIZE];

    // Add pipe to epoll
    struct epoll_event pipe_ev;
    pipe_ev.events = EPOLLIN;
    pipe_ev.data.fd = worker->pipe_fd[0];
    epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->pipe_fd[0], &pipe_ev);

    while (worker->running) {
        int n = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, 100);

        for (int i = 0; i < n; ++i) {
            // 1. Handle New Connection Dispatch (Pipe)
            if (events[i].data.fd == worker->pipe_fd[0]) {
                int client_fd;
                ssize_t r = read(worker->pipe_fd[0], &client_fd, sizeof(int));
                if (r == sizeof(int)) {
                    // Initialize new client
                    ws_connection_t* conn = calloc(1, sizeof(ws_connection_t));
                    ws_init(&conn->ws_client);

                    conn->epoll_fd = worker->epoll_fd;
                    conn->server = worker->server;
                    conn->worker = worker;
                    pthread_mutex_init(&conn->out_lock, NULL);
                    
                    // Do NOT set client->user_data to conn, let the user use it.
                    // We access conn via casting in write_cb and worker loop.
                    conn->ws_client.user_data = NULL;
                    
                    // We need to use the server's callbacks but wrapped.
                    // Since ws_client_t calls callbacks with itself as 1st arg,
                    // and we want to expose ws_client_t to the user, this is fine.
                    // But we need to set the global callbacks on the client struct.
                    conn->ws_client.on_open = worker->server->config.on_open;
                    conn->ws_client.on_message = worker->server->config.on_message;
                    conn->ws_client.on_close = worker->server->config.on_close;

                    // Set custom write
                    ws_set_write_cb(&conn->ws_client, server_write_cb);

                    // Handshake
                    if (ws_accept(&conn->ws_client, client_fd) != WS_OK) {
                        close(client_fd);
                        free(conn);
                        continue;
                    }

                    // Add to Epoll
                    make_non_blocking(client_fd);
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    ev.data.ptr = conn;
                    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        ws_cleanup(&conn->ws_client);
                        free(conn);
                        continue;
                    }

                    add_client_to_worker(worker, conn);

                    pthread_mutex_lock(&worker->server->stats_lock);
                    worker->server->total_clients++;
                    pthread_mutex_unlock(&worker->server->stats_lock);
                }
                continue;
            }

            // 2. Handle Client IO
            ws_connection_t* conn = (ws_connection_t*)events[i].data.ptr;
            int fd = conn->ws_client.socket_fd;
            uint32_t evs = events[i].events;

            // Handle Disconnects
            if (evs & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                // Remove from epoll first
                epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, fd, NULL);

                // Notify Close
                // Note: ws_close might have been called already?
                // ws_consume calls dispatch which calls on_close.
                // If we get here, it's a socket level disconnect.
                // We should ensure cleanup.
                // If state is not closed, call close.
                if (conn->ws_client.state != WS_STATE_CLOSED) {
                    // Synthetic close
                    if (conn->ws_client.on_close)
                        conn->ws_client.on_close(&conn->ws_client, WS_STATUS_ABNORMAL, "Connection reset");
                }

                pthread_mutex_lock(&worker->server->stats_lock);
                if (worker->server->total_clients > 0) worker->server->total_clients--;
                pthread_mutex_unlock(&worker->server->stats_lock);

                remove_client_from_worker(worker, conn);
                ws_cleanup(&conn->ws_client);

                // Free output buffer
                if (conn->out_buf) free(conn->out_buf);
                pthread_mutex_destroy(&conn->out_lock);

                free(conn);
                continue;
            }

            // Handle Read
            if (evs & EPOLLIN) {
                while (true) {
                    ssize_t count = read(fd, buffer, sizeof(buffer));
                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        // Error
                        break;
                    } else if (count == 0) {
                        // EOF handled by RDHUP usually, but just in case
                        break;
                    }

                    ws_consume(&conn->ws_client, buffer, (size_t)count);
                }
            }

            // Handle Write
            if (evs & EPOLLOUT) {
                pthread_mutex_lock(&conn->out_lock);
                if (conn->out_len > 0) {
                    ssize_t written = write(fd, conn->out_buf, conn->out_len);
                    if (written > 0) {
                        if ((size_t)written == conn->out_len) {
                            // Done
                            conn->out_len = 0;
                            // Stop watching OUT
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                            ev.data.ptr = conn;
                            epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        } else {
                            // Partial
                            memmove(conn->out_buf, conn->out_buf + written, conn->out_len - (size_t)written);
                            conn->out_len -= (size_t)written;
                        }
                    }
                }
                pthread_mutex_unlock(&conn->out_lock);
            }
        }
    }
    return NULL;
}

// --- Public API ---

ws_server_t* ws_server_create(ws_server_config_t* config) {
    ws_server_t* server = calloc(1, sizeof(ws_server_t));
    server->config = *config;
    if (server->config.thread_count <= 0) {
        server->config.thread_count = get_nprocs();
    }
    pthread_mutex_init(&server->stats_lock, NULL);
    return server;
}

void ws_server_start(ws_server_t* server) {
    // 1. Setup Listener
    struct sockaddr_in address;
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) return;

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    make_non_blocking(server->listen_fd);

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
    address.sin_port = htons(server->config.port);

    if (bind(server->listen_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return;
    }

    if (listen(server->listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        return;
    }

    printf("Server listening on port %d with %d threads\n", server->config.port, server->config.thread_count);

    // 2. Start Workers
    server->workers = calloc((size_t)server->config.thread_count, sizeof(ws_worker_t));
    server->worker_count = server->config.thread_count;
    server->running = true;

    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];
        w->server = server;
        w->id = i;
        w->running = true;
        w->epoll_fd = epoll_create1(0);
        pthread_mutex_init(&w->clients_lock, NULL);

        if (pipe(w->pipe_fd) < 0) {
            perror("pipe");
            // Handle error cleanup...
        }
        make_non_blocking(w->pipe_fd[0]);
        make_non_blocking(w->pipe_fd[1]);

        pthread_create(&w->thread, NULL, worker_routine, w);
    }

    // 3. Accept Loop (Main Thread)
    int main_epoll = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server->listen_fd;
    epoll_ctl(main_epoll, EPOLL_CTL_ADD, server->listen_fd, &ev);

    struct epoll_event events[16];
    int next_worker = 0;

    while (server->running) {
        int n = epoll_wait(main_epoll, events, 16, 500);  // 500ms timeout to check running

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server->listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);
                int client_fd = accept(server->listen_fd, (struct sockaddr*)&client_addr, &len);

                if (client_fd >= 0) {
                    // Dispatch to worker (Round Robin)
                    ws_worker_t* w = &server->workers[next_worker];
                    write(w->pipe_fd[1], &client_fd, sizeof(int));

                    next_worker = (next_worker + 1) % server->worker_count;
                }
            }
        }
    }

    close(main_epoll);
    close(server->listen_fd);
    // Join threads... (implemented in destroy/stop)
}

void ws_server_stop(ws_server_t* server) { server->running = false; }

void ws_server_destroy(ws_server_t* server) {
    server->running = false;

    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];
        w->running = false;
        pthread_join(w->thread, NULL);

        close(w->epoll_fd);
        close(w->pipe_fd[0]);
        close(w->pipe_fd[1]);

        pthread_mutex_destroy(&w->clients_lock);
        free(w->clients);  // Note: clients themselves should be cleaned up by worker loop or here
    }

    free(server->workers);
    pthread_mutex_destroy(&server->stats_lock);
    free(server);
}

void ws_server_broadcast_text(ws_server_t* server, const char* msg) {
    ws_server_broadcast_text_filter(server, msg, NULL, NULL);
}

void ws_server_broadcast_binary(ws_server_t* server, const uint8_t* data, size_t len) {
    ws_server_broadcast_binary_filter(server, data, len, NULL, NULL);
}

void ws_server_broadcast_text_filter(ws_server_t* server, const char* msg, ws_filter_cb_t filter, void* arg) {
    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];
        pthread_mutex_lock(&w->clients_lock);
        for (size_t j = 0; j < w->client_count; ++j) {
            ws_client_t* client = &w->clients[j]->ws_client;
            if (!filter || filter(client, arg)) {
                ws_send_text(client, msg);
            }
        }
        pthread_mutex_unlock(&w->clients_lock);
    }
}

void ws_server_broadcast_binary_filter(ws_server_t* server, const uint8_t* data, size_t len, ws_filter_cb_t filter,
                                       void* arg) {
    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];
        pthread_mutex_lock(&w->clients_lock);
        for (size_t j = 0; j < w->client_count; ++j) {
            ws_client_t* client = &w->clients[j]->ws_client;
            if (!filter || filter(client, arg)) {
                ws_send_binary(client, data, len);
            }
        }
        pthread_mutex_unlock(&w->clients_lock);
    }
}

size_t ws_server_get_client_count(ws_server_t* server) {
    pthread_mutex_lock(&server->stats_lock);
    size_t c = server->total_clients;
    pthread_mutex_unlock(&server->stats_lock);
    return c;
}
