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

#define MAX_EVENTS        64
#define READ_BUFFER_SIZE  4096
#define MAX_OUTPUT_BUFFER (1024 * 1024 * 10)  // 10MB max output buffer per client

// --- Internal Structures ---

// Connection state flags
typedef enum { CONN_STATE_ACTIVE = 0, CONN_STATE_CLOSING = 1, CONN_STATE_CLOSED = 2 } conn_state_t;

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

    // State tracking
    volatile conn_state_t state;
    bool in_worker_list;  // Track if added to worker's client list
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

    bool initialized;  // Track if worker was fully initialized
} ws_worker_t;

// The main server context
struct ws_server_s {
    ws_server_config_t config;
    int listen_fd;
    volatile bool running;

    ws_worker_t* workers;
    int worker_count;

    // SSL
    SSL_CTX* ssl_ctx;

    // Global stats
    pthread_mutex_t stats_lock;
    size_t total_clients;
};

// --- Helper Functions ---

static int make_non_blocking(int fd) {
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void add_client_to_worker(ws_worker_t* worker, ws_connection_t* conn) {
    if (!worker || !conn) return;

    pthread_mutex_lock(&worker->clients_lock);

    if (worker->client_count >= worker->client_cap) {
        size_t new_cap = worker->client_cap == 0 ? 16 : worker->client_cap * 2;
        ws_connection_t** new_list = realloc(worker->clients, new_cap * sizeof(ws_connection_t*));
        if (!new_list) {
            pthread_mutex_unlock(&worker->clients_lock);
            return;  // Failed to add, but don't crash
        }
        worker->clients = new_list;
        worker->client_cap = new_cap;
    }

    if (worker->client_count < worker->client_cap) {
        worker->clients[worker->client_count++] = conn;
        conn->in_worker_list = true;
    }

    pthread_mutex_unlock(&worker->clients_lock);
}

static void remove_client_from_worker(ws_worker_t* worker, ws_connection_t* conn) {
    if (!worker || !conn) return;

    pthread_mutex_lock(&worker->clients_lock);

    if (!conn->in_worker_list) {
        pthread_mutex_unlock(&worker->clients_lock);
        return;
    }

    for (size_t i = 0; i < worker->client_count; ++i) {
        if (worker->clients[i] == conn) {
            // Swap with last
            worker->clients[i] = worker->clients[worker->client_count - 1];
            worker->client_count--;
            conn->in_worker_list = false;
            break;
        }
    }

    pthread_mutex_unlock(&worker->clients_lock);
}

// Safe connection cleanup - can be called from multiple contexts
static void cleanup_connection(ws_connection_t* conn) {
    if (!conn) return;

    // Mark as closed to prevent further operations
    conn->state = CONN_STATE_CLOSED;

    // Remove from epoll first to prevent new events
    if (conn->epoll_fd >= 0 && conn->ws_client.socket_fd >= 0) {
        epoll_ctl(conn->epoll_fd, EPOLL_CTL_DEL, conn->ws_client.socket_fd, NULL);
    }

    // Remove from worker's client list
    if (conn->worker) {
        remove_client_from_worker(conn->worker, conn);
    }

    // Update stats
    if (conn->server) {
        pthread_mutex_lock(&conn->server->stats_lock);
        if (conn->server->total_clients > 0) {
            conn->server->total_clients--;
        }
        pthread_mutex_unlock(&conn->server->stats_lock);
    }

    // Cleanup WebSocket client
    ws_cleanup(&conn->ws_client);

    // Free output buffer
    pthread_mutex_lock(&conn->out_lock);
    if (conn->out_buf) {
        free(conn->out_buf);
        conn->out_buf = NULL;
    }
    conn->out_len = 0;
    conn->out_cap = 0;
    pthread_mutex_unlock(&conn->out_lock);

    pthread_mutex_destroy(&conn->out_lock);

    // Finally free the connection structure
    free(conn);
}

// Custom Write Callback for Non-Blocking I/O
static int server_write_cb(ws_client_t* client, const uint8_t* data, size_t len) {
    if (!client || !data || len == 0) return -1;

    ws_connection_t* conn = (ws_connection_t*)client;

    // Check if connection is closing/closed
    if (conn->state != CONN_STATE_ACTIVE) {
        return -1;
    }

    pthread_mutex_lock(&conn->out_lock);

    // Try to write directly if buffer is empty
    size_t written = 0;
    if (conn->out_len == 0) {
        ssize_t n;
        if (client->use_ssl && client->ssl) {
            int r = SSL_write(client->ssl, data, (int)len);
            if (r > 0) {
                n = r;
            } else {
                int err = SSL_get_error(client->ssl, r);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    n = -1;
                    errno = EAGAIN;
                } else {
                    n = -1;
                    errno = EIO;
                }
            }
        } else {
            n = send(client->socket_fd, data, len, MSG_NOSIGNAL);
        }

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

        // Check for buffer overflow
        if (conn->out_len + remaining > MAX_OUTPUT_BUFFER) {
            pthread_mutex_unlock(&conn->out_lock);
            return -1;  // Buffer too full
        }

        if (conn->out_len + remaining > conn->out_cap) {
            size_t new_cap = conn->out_cap == 0 ? 4096 : conn->out_cap * 2;

            // Prevent integer overflow
            while (new_cap < conn->out_len + remaining) {
                if (new_cap > MAX_OUTPUT_BUFFER / 2) {
                    new_cap = MAX_OUTPUT_BUFFER;
                    break;
                }
                new_cap *= 2;
            }

            if (new_cap > MAX_OUTPUT_BUFFER) {
                new_cap = MAX_OUTPUT_BUFFER;
            }

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
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
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
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->pipe_fd[0], &pipe_ev) < 0) {
        perror("epoll_ctl pipe");
        return NULL;
    }

    while (worker->running) {
        int timeout_ms = worker->server->config.timeout_ms > 0 ? worker->server->config.timeout_ms : 100;
        int n = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, timeout_ms);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            // 1. Handle New Connection Dispatch (Pipe)
            if (events[i].data.fd == worker->pipe_fd[0]) {
                int client_fd;
                ssize_t r = read(worker->pipe_fd[0], &client_fd, sizeof(int));
                if (r != sizeof(int)) continue;

                // Initialize new client
                ws_connection_t* conn = calloc(1, sizeof(ws_connection_t));
                if (!conn) {
                    close(client_fd);
                    continue;
                }

                ws_init(&conn->ws_client);

                conn->epoll_fd = worker->epoll_fd;
                conn->server = worker->server;
                conn->worker = worker;
                conn->state = CONN_STATE_ACTIVE;
                conn->in_worker_list = false;
                pthread_mutex_init(&conn->out_lock, NULL);

                // Set user_data to NULL - user can set it in their callbacks
                conn->ws_client.user_data = NULL;

                // Set server callbacks
                conn->ws_client.on_open = worker->server->config.on_open;
                conn->ws_client.on_message = worker->server->config.on_message;
                conn->ws_client.on_close = worker->server->config.on_close;

                // Setup SSL if configured
                if (worker->server->ssl_ctx) {
                    conn->ws_client.use_ssl = true;
                    conn->ws_client.ssl_ctx = worker->server->ssl_ctx;

                    // Increment ref count (returns void, no error check needed)
                    SSL_CTX_up_ref(conn->ws_client.ssl_ctx);

                    conn->ws_client.ssl = SSL_new(worker->server->ssl_ctx);
                    if (!conn->ws_client.ssl) {
                        ws_cleanup(&conn->ws_client);
                        pthread_mutex_destroy(&conn->out_lock);
                        close(client_fd);
                        free(conn);
                        continue;
                    }

                    if (SSL_set_fd(conn->ws_client.ssl, client_fd) != 1) {
                        ws_cleanup(&conn->ws_client);
                        pthread_mutex_destroy(&conn->out_lock);
                        close(client_fd);
                        free(conn);
                        continue;
                    }

                    // Enable partial writes for non-blocking buffer logic
                    SSL_set_mode(conn->ws_client.ssl,
                                 SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

                    SSL_set_accept_state(conn->ws_client.ssl);
                }

                // Set custom write callback
                ws_set_write_cb(&conn->ws_client, server_write_cb);

                // Accept WebSocket connection
                if (ws_accept(&conn->ws_client, client_fd) != WS_OK) {
                    ws_cleanup(&conn->ws_client);
                    pthread_mutex_destroy(&conn->out_lock);
                    // ws_accept sets socket_fd, ws_cleanup will close it
                    free(conn);
                    continue;
                }

                // Make socket non-blocking
                if (make_non_blocking(client_fd) < 0) {
                    ws_cleanup(&conn->ws_client);
                    pthread_mutex_destroy(&conn->out_lock);
                    free(conn);
                    continue;
                }

                // Add to Epoll
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                ev.data.ptr = conn;
                if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                    ws_cleanup(&conn->ws_client);
                    pthread_mutex_destroy(&conn->out_lock);
                    free(conn);
                    continue;
                }

                // Add to worker's client list
                add_client_to_worker(worker, conn);

                // Update stats
                pthread_mutex_lock(&worker->server->stats_lock);
                worker->server->total_clients++;
                pthread_mutex_unlock(&worker->server->stats_lock);

                continue;
            }

            // 2. Handle Client IO
            ws_connection_t* conn = (ws_connection_t*)events[i].data.ptr;
            if (!conn) continue;

            // Skip if already closing/closed
            if (conn->state != CONN_STATE_ACTIVE) continue;

            int fd = conn->ws_client.socket_fd;
            uint32_t evs = events[i].events;

            // Handle Disconnects
            if (evs & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                if (conn->state == CONN_STATE_ACTIVE) {
                    conn->state = CONN_STATE_CLOSING;

                    // Notify close callback if not already closed at protocol level
                    if (conn->ws_client.state != WS_STATE_CLOSED) {
                        if (conn->ws_client.on_close) {
                            conn->ws_client.on_close(&conn->ws_client, WS_STATUS_ABNORMAL, "Connection reset");
                        }
                    }

                    cleanup_connection(conn);
                }
                continue;
            }

            // Handle Read
            if (evs & EPOLLIN) {
                bool read_error = false;
                while (true) {
                    ssize_t count = ws_read(&conn->ws_client, buffer, sizeof(buffer));
                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // No more data
                        }
                        // Read error
                        read_error = true;
                        break;
                    } else if (count == 0) {
                        // EOF
                        read_error = true;
                        break;
                    }

                    // Process received data
                    ws_error_t err = ws_consume(&conn->ws_client, buffer, (size_t)count);
                    if (err != WS_OK) {
                        read_error = true;
                        break;
                    }

                    // Check if underlying client state is closed (e.g. after receiving Close frame)
                    if (conn->ws_client.state == WS_STATE_CLOSED) {
                        pthread_mutex_lock(&conn->out_lock);
                        bool pending = (conn->out_len > 0);
                        pthread_mutex_unlock(&conn->out_lock);

                        if (pending) {
                            conn->state = CONN_STATE_CLOSING;
                            break;
                        } else {
                            read_error = true; // Trigger cleanup
                            break;
                        }
                    }

                    // Check if connection was closed during processing (by user callback?)
                    if (conn->state != CONN_STATE_ACTIVE) {
                        break;
                    }
                }

                if (read_error && conn->state == CONN_STATE_ACTIVE) {
                    conn->state = CONN_STATE_CLOSING;
                    cleanup_connection(conn);
                    continue;
                }
            }

            // Handle Write
            if ((evs & EPOLLOUT) && (conn->state == CONN_STATE_ACTIVE || conn->state == CONN_STATE_CLOSING)) {
                pthread_mutex_lock(&conn->out_lock);

                if (conn->out_len > 0) {
                    ssize_t written;
                    if (conn->ws_client.use_ssl && conn->ws_client.ssl) {
                        int r = SSL_write(conn->ws_client.ssl, conn->out_buf, (int)conn->out_len);
                        if (r > 0) {
                            written = r;
                        } else {
                            int err = SSL_get_error(conn->ws_client.ssl, r);
                            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                                written = -1;
                                errno = EAGAIN;
                            } else {
                                written = -1;
                                errno = EIO;
                            }
                        }
                    } else {
                        written = send(fd, conn->out_buf, conn->out_len, MSG_NOSIGNAL);
                    }

                    if (written > 0) {
                        if ((size_t)written == conn->out_len) {
                            // All data sent
                            conn->out_len = 0;

                            if (conn->state == CONN_STATE_CLOSING) {
                                pthread_mutex_unlock(&conn->out_lock);
                                cleanup_connection(conn);
                                continue;
                            }

                            // Stop watching EPOLLOUT
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                            ev.data.ptr = conn;
                            epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        } else {
                            // Partial write
                            memmove(conn->out_buf, conn->out_buf + written, conn->out_len - (size_t)written);
                            conn->out_len -= (size_t)written;
                        }
                    } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        // Write error
                        pthread_mutex_unlock(&conn->out_lock);
                        conn->state = CONN_STATE_CLOSING;
                        cleanup_connection(conn);
                        continue;
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
    if (!config) return NULL;

    ws_server_t* server = calloc(1, sizeof(ws_server_t));
    if (!server) return NULL;

    server->config = *config;
    server->listen_fd = -1;

    if (server->config.thread_count <= 0) {
        server->config.thread_count = get_nprocs();
        if (server->config.thread_count <= 0) {
            server->config.thread_count = 4;  // Fallback
        }
    }

    if (server->config.timeout_ms <= 0) {
        server->config.timeout_ms = 100;
    }

    pthread_mutex_init(&server->stats_lock, NULL);

    // Init SSL if configured
    if (server->config.ssl_cert && server->config.ssl_key) {
        server->ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!server->ssl_ctx) {
            fprintf(stderr, "Failed to create SSL context\n");
            ERR_print_errors_fp(stderr);
            pthread_mutex_destroy(&server->stats_lock);
            free(server);
            return NULL;
        }

        if (SSL_CTX_use_certificate_file(server->ssl_ctx, server->config.ssl_cert, SSL_FILETYPE_PEM) <= 0) {
            fprintf(stderr, "Failed to load cert file: %s\n", server->config.ssl_cert);
            ERR_print_errors_fp(stderr);
            SSL_CTX_free(server->ssl_ctx);
            pthread_mutex_destroy(&server->stats_lock);
            free(server);
            return NULL;
        }

        if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, server->config.ssl_key, SSL_FILETYPE_PEM) <= 0) {
            fprintf(stderr, "Failed to load key file: %s\n", server->config.ssl_key);
            ERR_print_errors_fp(stderr);
            SSL_CTX_free(server->ssl_ctx);
            pthread_mutex_destroy(&server->stats_lock);
            free(server);
            return NULL;
        }

        // Verify that certificate and key match
        if (SSL_CTX_check_private_key(server->ssl_ctx) != 1) {
            fprintf(stderr, "Certificate and private key do not match\n");
            ERR_print_errors_fp(stderr);
            SSL_CTX_free(server->ssl_ctx);
            pthread_mutex_destroy(&server->stats_lock);
            free(server);
            return NULL;
        }
    }

    return server;
}

void ws_server_start(ws_server_t* server) {
    if (!server) return;

    // 1. Setup Listener
    struct sockaddr_in address;
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        perror("socket");
        return;
    }

    int opt = 1;
    if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }

    if (make_non_blocking(server->listen_fd) < 0) {
        perror("make_non_blocking listen_fd");
        close(server->listen_fd);
        server->listen_fd = -1;
        return;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server->config.port);

    if (bind(server->listen_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server->listen_fd);
        server->listen_fd = -1;
        return;
    }

    if (listen(server->listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server->listen_fd);
        server->listen_fd = -1;
        return;
    }

    printf("Server listening on port %d with %d threads\n", server->config.port, server->config.thread_count);

    // 2. Start Workers
    server->workers = calloc((size_t)server->config.thread_count, sizeof(ws_worker_t));
    if (!server->workers) {
        perror("calloc workers");
        close(server->listen_fd);
        server->listen_fd = -1;
        return;
    }

    server->worker_count = server->config.thread_count;
    server->running = true;

    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];
        w->server = server;
        w->id = i;
        w->running = true;
        w->initialized = false;
        w->pipe_fd[0] = -1;
        w->pipe_fd[1] = -1;
        w->epoll_fd = -1;
        w->clients = NULL;
        w->client_count = 0;
        w->client_cap = 0;

        w->epoll_fd = epoll_create1(0);
        if (w->epoll_fd < 0) {
            perror("epoll_create1");
            // Cleanup already initialized workers
            for (int j = 0; j < i; ++j) {
                server->workers[j].running = false;
            }
            goto cleanup_workers;
        }

        pthread_mutex_init(&w->clients_lock, NULL);

        if (pipe(w->pipe_fd) < 0) {
            perror("pipe");
            close(w->epoll_fd);
            pthread_mutex_destroy(&w->clients_lock);
            // Cleanup already initialized workers
            for (int j = 0; j < i; ++j) {
                server->workers[j].running = false;
            }
            goto cleanup_workers;
        }

        if (make_non_blocking(w->pipe_fd[0]) < 0 || make_non_blocking(w->pipe_fd[1]) < 0) {
            perror("make_non_blocking pipe");
            close(w->pipe_fd[0]);
            close(w->pipe_fd[1]);
            close(w->epoll_fd);
            pthread_mutex_destroy(&w->clients_lock);
            // Cleanup already initialized workers
            for (int j = 0; j < i; ++j) {
                server->workers[j].running = false;
            }
            goto cleanup_workers;
        }

        if (pthread_create(&w->thread, NULL, worker_routine, w) != 0) {
            perror("pthread_create");
            close(w->pipe_fd[0]);
            close(w->pipe_fd[1]);
            close(w->epoll_fd);
            pthread_mutex_destroy(&w->clients_lock);
            // Cleanup already initialized workers
            for (int j = 0; j < i; ++j) {
                server->workers[j].running = false;
            }
            goto cleanup_workers;
        }

        w->initialized = true;
    }

    // 3. Accept Loop (Main Thread)
    int main_epoll = epoll_create1(0);
    if (main_epoll < 0) {
        perror("epoll_create1 main");
        goto cleanup_workers;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server->listen_fd;
    if (epoll_ctl(main_epoll, EPOLL_CTL_ADD, server->listen_fd, &ev) < 0) {
        perror("epoll_ctl listen_fd");
        close(main_epoll);
        goto cleanup_workers;
    }

    struct epoll_event events[16];
    int next_worker = 0;

    while (server->running) {
        int n = epoll_wait(main_epoll, events, 16, 500);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait main");
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server->listen_fd) {
                // Accept all pending connections
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(server->listen_fd, (struct sockaddr*)&client_addr, &len);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // No more connections
                        }
                        perror("accept");
                        break;
                    }

                    // Dispatch to worker (Round Robin)
                    ws_worker_t* w = &server->workers[next_worker];
                    ssize_t written = write(w->pipe_fd[1], &client_fd, sizeof(int));
                    if (written != sizeof(int)) {
                        perror("write to worker pipe");
                        close(client_fd);
                    }

                    next_worker = (next_worker + 1) % server->worker_count;
                }
            }
        }
    }

    close(main_epoll);
    close(server->listen_fd);
    server->listen_fd = -1;

cleanup_workers:
    // Signal all workers to stop
    for (int i = 0; i < server->worker_count; ++i) {
        server->workers[i].running = false;
    }

    // Join all initialized workers
    for (int i = 0; i < server->worker_count; ++i) {
        if (server->workers[i].initialized) {
            pthread_join(server->workers[i].thread, NULL);
        }
    }

    // Cleanup workers
    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];

        // Close file descriptors
        if (w->epoll_fd >= 0) close(w->epoll_fd);
        if (w->pipe_fd[0] >= 0) close(w->pipe_fd[0]);
        if (w->pipe_fd[1] >= 0) close(w->pipe_fd[1]);

        // Cleanup remaining clients
        pthread_mutex_lock(&w->clients_lock);
        for (size_t j = 0; j < w->client_count; ++j) {
            ws_connection_t* conn = w->clients[j];
            if (conn) {
                conn->in_worker_list = false;  // Prevent removal attempt
                cleanup_connection(conn);
            }
        }
        if (w->clients) {
            free(w->clients);
            w->clients = NULL;
        }
        w->client_count = 0;
        pthread_mutex_unlock(&w->clients_lock);

        pthread_mutex_destroy(&w->clients_lock);
    }

    if (server->workers) {
        free(server->workers);
        server->workers = NULL;
    }
    server->worker_count = 0;
}

void ws_server_stop(ws_server_t* server) {
    if (!server) return;
    server->running = false;
}

void ws_server_destroy(ws_server_t* server) {
    if (!server) return;

    // Stop if still running
    server->running = false;

    // Workers should be cleaned up by ws_server_start before returning
    // But just in case:
    if (server->workers) {
        for (int i = 0; i < server->worker_count; ++i) {
            ws_worker_t* w = &server->workers[i];
            w->running = false;
            if (w->initialized) {
                pthread_join(w->thread, NULL);
            }
            if (w->clients) {
                free(w->clients);
            }
            pthread_mutex_destroy(&w->clients_lock);
        }
        free(server->workers);
        server->workers = NULL;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    pthread_mutex_destroy(&server->stats_lock);

    if (server->ssl_ctx) {
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
    }

    free(server);
}

void ws_server_broadcast_text(ws_server_t* server, const char* msg) {
    ws_server_broadcast_text_filter(server, msg, NULL, NULL);
}

void ws_server_broadcast_binary(ws_server_t* server, const uint8_t* data, size_t len) {
    ws_server_broadcast_binary_filter(server, data, len, NULL, NULL);
}

void ws_server_broadcast_text_filter(ws_server_t* server, const char* msg, ws_filter_cb_t filter, void* arg) {
    if (!server || !msg) return;

    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];

        pthread_mutex_lock(&w->clients_lock);

        // Create a snapshot of clients to avoid holding lock during send
        size_t count = w->client_count;
        ws_connection_t** snapshot = NULL;
        if (count > 0) {
            snapshot = malloc(count * sizeof(ws_connection_t*));
            if (snapshot) {
                memcpy(snapshot, w->clients, count * sizeof(ws_connection_t*));
            }
        }

        pthread_mutex_unlock(&w->clients_lock);

        if (snapshot) {
            for (size_t j = 0; j < count; ++j) {
                ws_connection_t* conn = snapshot[j];
                if (conn && conn->state == CONN_STATE_ACTIVE) {
                    ws_client_t* client = &conn->ws_client;
                    if (client->state == WS_STATE_OPEN) {
                        if (!filter || filter(client, arg)) {
                            ws_send_text(client, msg);
                        }
                    }
                }
            }
            free(snapshot);
        }
    }
}

void ws_server_broadcast_binary_filter(ws_server_t* server, const uint8_t* data, size_t len, ws_filter_cb_t filter,
                                       void* arg) {
    if (!server || !data) return;

    for (int i = 0; i < server->worker_count; ++i) {
        ws_worker_t* w = &server->workers[i];

        pthread_mutex_lock(&w->clients_lock);

        // Create a snapshot of clients
        size_t count = w->client_count;
        ws_connection_t** snapshot = NULL;
        if (count > 0) {
            snapshot = malloc(count * sizeof(ws_connection_t*));
            if (snapshot) {
                memcpy(snapshot, w->clients, count * sizeof(ws_connection_t*));
            }
        }

        pthread_mutex_unlock(&w->clients_lock);

        if (snapshot) {
            for (size_t j = 0; j < count; ++j) {
                ws_connection_t* conn = snapshot[j];
                if (conn && conn->state == CONN_STATE_ACTIVE) {
                    ws_client_t* client = &conn->ws_client;
                    if (client->state == WS_STATE_OPEN) {
                        if (!filter || filter(client, arg)) {
                            ws_send_binary(client, data, len);
                        }
                    }
                }
            }
            free(snapshot);
        }
    }
}

size_t ws_server_get_client_count(ws_server_t* server) {
    if (!server) return 0;

    pthread_mutex_lock(&server->stats_lock);
    size_t c = server->total_clients;
    pthread_mutex_unlock(&server->stats_lock);
    return c;
}
