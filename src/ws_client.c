#include "../include/ws_client.h"
#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// Helper Functions
// ============================================================================

static time_t get_current_time(void) { return time(NULL); }

static int64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// ============================================================================
// Configuration
// ============================================================================

ws_client_config_t ws_client_default_config(void) {
    ws_client_config_t config = {
        .host = "localhost",
        .port = 80,
        .path = "/",
        .use_ssl = false,

        .connect_timeout = 10,
        .read_timeout = 0,    // No timeout by default
        .ping_interval = 30,  // Ping every 30 seconds
        .pong_timeout = 10,   // Expect pong within 10 seconds

        .auto_reconnect = false,
        .max_reconnect_attempts = 0,
        .reconnect_delay_ms = 1000,

        .non_blocking = false,
        .read_buffer_size = 4096,
    };
    return config;
}

void ws_client_config_set_timeouts_realtime(ws_client_config_t* config) {
    if (!config) return;
    config->connect_timeout = 5;
    config->read_timeout = 0;
    config->ping_interval = 10;
    config->pong_timeout = 5;
}

void ws_client_config_set_timeouts_standard(ws_client_config_t* config) {
    if (!config) return;
    config->connect_timeout = 10;
    config->read_timeout = 0;
    config->ping_interval = 30;
    config->pong_timeout = 10;
}

void ws_client_config_set_timeouts_relaxed(ws_client_config_t* config) {
    if (!config) return;
    config->connect_timeout = 30;
    config->read_timeout = 0;
    config->ping_interval = 60;
    config->pong_timeout = 30;
}

// ============================================================================
// Core API
// ============================================================================

void ws_client_init(ws_client_context_t* ctx) {
    if (!ctx) return;

    memset(ctx, 0, sizeof(ws_client_context_t));
    ws_init(&ctx->ws_client);

    ctx->config = ws_client_default_config();
    ctx->running = false;
    ctx->should_reconnect = false;
    ctx->reconnect_attempts = 0;
    ctx->thread_running = false;
}

void ws_client_configure(ws_client_context_t* ctx, ws_client_config_t* config) {
    if (!ctx || !config) return;
    ctx->config = *config;

    // Apply configuration to underlying client
    ws_set_ssl(&ctx->ws_client, config->use_ssl);
}

ws_error_t ws_client_connect(ws_client_context_t* ctx) {
    if (!ctx) return WS_ERR_INVALID_PARAMETER;

    // Check if already connected
    if (ctx->ws_client.state == WS_STATE_OPEN || ctx->ws_client.state == WS_STATE_CONNECTING) {
        return WS_ERR_INVALID_STATE;
    }

    // Ensure client is in clean state
    if (ctx->ws_client.state != WS_STATE_CLOSED) {
        ws_cleanup(&ctx->ws_client);
        ws_init(&ctx->ws_client);
    }

    ctx->connection_started = get_current_time();
    ctx->last_ping_sent = 0;
    ctx->last_pong_received = get_current_time();

    // Attempt connection
    ws_error_t err = ws_connect(&ctx->ws_client, ctx->config.host, ctx->config.port, ctx->config.path);
    if (err != WS_OK) {
        return err;
    }

    // Wait for handshake completion with timeout
    time_t start = get_current_time();
    uint8_t buffer[4096];

    while (ctx->ws_client.state == WS_STATE_CONNECTING) {
        // Check timeout
        if (ctx->config.connect_timeout > 0) {
            time_t elapsed = get_current_time() - start;
            if (elapsed >= ctx->config.connect_timeout) {
                ws_cleanup(&ctx->ws_client);
                ws_init(&ctx->ws_client);
                return WS_ERR_CONNECT_FAILED;
            }
        }

        // Wait for data with select
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->ws_client.socket_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(ctx->ws_client.socket_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ws_cleanup(&ctx->ws_client);
            ws_init(&ctx->ws_client);
            return WS_ERR_IO_ERROR;
        }

        if (ret == 0) continue;  // Timeout, loop again

        // Read and process handshake data
        ssize_t n = ws_read(&ctx->ws_client, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            ws_cleanup(&ctx->ws_client);
            ws_init(&ctx->ws_client);
            return WS_ERR_IO_ERROR;
        }

        if (n == 0) {
            ws_cleanup(&ctx->ws_client);
            ws_init(&ctx->ws_client);
            return WS_ERR_CONNECT_FAILED;
        }

        ws_error_t consume_err = ws_consume(&ctx->ws_client, buffer, (size_t)n);
        if (consume_err != WS_OK) {
            ws_cleanup(&ctx->ws_client);
            ws_init(&ctx->ws_client);
            return consume_err;
        }
    }

    // Check if successfully connected
    if (ctx->ws_client.state != WS_STATE_OPEN) {
        ws_cleanup(&ctx->ws_client);
        ws_init(&ctx->ws_client);
        return WS_ERR_HANDSHAKE_FAILED;
    }

    ctx->reconnect_attempts = 0;  // Reset on successful connect
    return WS_OK;
}

void ws_client_disconnect(ws_client_context_t* ctx) {
    if (!ctx) return;

    ctx->running = false;
    ctx->should_reconnect = false;

    if (ctx->ws_client.state == WS_STATE_OPEN || ctx->ws_client.state == WS_STATE_CONNECTING) {
        ws_close(&ctx->ws_client, WS_STATUS_NORMAL, "Client disconnect");

        // Give server time to acknowledge close
        sleep_ms(100);
    }
}

void ws_client_cleanup(ws_client_context_t* ctx) {
    if (!ctx) return;

    // Stop if running
    ctx->running = false;
    ctx->should_reconnect = false;

    // Stop thread if running
    if (ctx->thread_running) {
        ws_client_stop_thread(ctx);
    }

    // Cleanup WebSocket client
    ws_cleanup(&ctx->ws_client);
}

// ============================================================================
// Event Processing
// ============================================================================

static int process_events(ws_client_context_t* ctx, int timeout_ms) {
    if (!ctx) return -1;
    if (ctx->ws_client.state == WS_STATE_CLOSED) return 0;

    int events_processed = 0;
    int64_t start_time = get_time_ms();
    size_t buffer_size = ctx->config.read_buffer_size > 0 ? ctx->config.read_buffer_size : 4096;
    uint8_t* buffer = malloc(buffer_size);
    if (!buffer) return -1;

    while (ctx->running) {
        // Check timeout
        if (timeout_ms >= 0) {
            int64_t elapsed = get_time_ms() - start_time;
            if (elapsed >= timeout_ms) break;
        }

        // Handle periodic tasks (ping, timeout checks)
        time_t now = get_current_time();

        // Send ping if needed
        if (ctx->config.ping_interval > 0 && ctx->ws_client.state == WS_STATE_OPEN) {
            time_t since_last_ping = now - ctx->last_ping_sent;
            if (since_last_ping >= ctx->config.ping_interval) {
                ws_send_ping(&ctx->ws_client, NULL, 0);
                ctx->last_ping_sent = now;
            }
        }

        // Check pong timeout
        if (ctx->config.pong_timeout > 0 && ctx->last_ping_sent > 0) {
            time_t since_last_pong = now - ctx->last_pong_received;
            if (since_last_pong > ctx->config.pong_timeout + ctx->config.ping_interval) {
                // No pong received, connection may be dead
                if (ctx->ws_client.on_error) {
                    ctx->ws_client.on_error(&ctx->ws_client, "Pong timeout");
                }
                ws_close(&ctx->ws_client, WS_STATUS_ABNORMAL, "Pong timeout");
                free(buffer);
                return -1;
            }
        }

        // Wait for data with select
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->ws_client.socket_fd, &readfds);

        struct timeval tv;
        int remaining_ms = timeout_ms >= 0 ? (timeout_ms - (int)(get_time_ms() - start_time)) : 100;
        if (remaining_ms < 0) remaining_ms = 0;
        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;

        int ret = select(ctx->ws_client.socket_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            return -1;
        }

        if (ret == 0) {
            // Timeout or no data
            if (timeout_ms >= 0) break;
            continue;
        }

        // Read data
        ssize_t n = ws_read(&ctx->ws_client, buffer, buffer_size);

        if (n < 0) {
            if (ctx->ws_client.use_ssl && ctx->ws_client.ssl) {
                int err = SSL_get_error(ctx->ws_client.ssl, (int)n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;

                if (ctx->ws_client.on_error) {
                    ctx->ws_client.on_error(&ctx->ws_client, "SSL Read error");
                }
            } else {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;

                if (ctx->ws_client.on_error) {
                    ctx->ws_client.on_error(&ctx->ws_client, "Read error");
                }
            }
            free(buffer);
            return -1;
        }

        if (n == 0) {
            // EOF
            if (ctx->ws_client.on_close) {
                ctx->ws_client.on_close(&ctx->ws_client, WS_STATUS_ABNORMAL, "Connection closed by peer");
            }
            free(buffer);
            return -1;
        }

        // Process received data
        ws_error_t err = ws_consume(&ctx->ws_client, buffer, (size_t)n);
        if (err != WS_OK) {
            if (ctx->ws_client.on_error) {
                ctx->ws_client.on_error(&ctx->ws_client, ws_strerror(err));
            }
            free(buffer);
            return -1;
        }

        events_processed++;

        // Check if connection closed during processing
        if (ctx->ws_client.state == WS_STATE_CLOSED) {
            break;
        }
    }

    free(buffer);
    return events_processed;
}

// ============================================================================
// Run Modes
// ============================================================================

void ws_client_run(ws_client_context_t* ctx) {
    if (!ctx) return;

    ctx->running = true;

    while (ctx->running && ctx->ws_client.state != WS_STATE_CLOSED) {
        int ret = process_events(ctx, -1);  // Block indefinitely

        if (ret < 0) {
            // Error or disconnect
            if (ctx->config.auto_reconnect && ctx->should_reconnect) {
                // Attempt reconnection
                if (ctx->config.max_reconnect_attempts == 0 ||
                    ctx->reconnect_attempts < ctx->config.max_reconnect_attempts) {
                    int delay = ctx->config.reconnect_delay_ms * (1 << ctx->reconnect_attempts);
                    if (delay > 60000) delay = 60000;  // Max 60 seconds

                    fprintf(stderr, "Reconnecting in %d ms (attempt %d)...\n", delay, ctx->reconnect_attempts + 1);

                    sleep_ms(delay);

                    ws_cleanup(&ctx->ws_client);
                    ws_init(&ctx->ws_client);

                    // Restore callbacks (they were cleared by ws_init)
                    // User should have set them before calling run

                    ws_error_t err = ws_client_connect(ctx);
                    if (err == WS_OK) {
                        ctx->reconnect_count++;
                        fprintf(stderr, "Reconnected successfully!\n");
                        continue;
                    }

                    ctx->reconnect_attempts++;
                    fprintf(stderr, "Reconnection failed: %s\n", ws_strerror(err));
                } else {
                    fprintf(stderr, "Max reconnection attempts reached\n");
                    break;
                }
            } else {
                break;
            }
        }
    }

    ctx->running = false;
}

int ws_client_poll(ws_client_context_t* ctx, int timeout_ms) {
    if (!ctx) return -1;

    if (ctx->ws_client.state == WS_STATE_CLOSED) {
        return 0;
    }

    return process_events(ctx, timeout_ms);
}

static void* client_thread_routine(void* arg) {
    ws_client_context_t* ctx = (ws_client_context_t*)arg;
    ws_client_run(ctx);
    return NULL;
}

ws_error_t ws_client_start_thread(ws_client_context_t* ctx) {
    if (!ctx) return WS_ERR_INVALID_PARAMETER;

    if (ctx->thread_running) {
        return WS_ERR_INVALID_STATE;
    }

    ctx->running = true;
    ctx->should_reconnect = ctx->config.auto_reconnect;

    if (pthread_create(&ctx->thread, NULL, client_thread_routine, ctx) != 0) {
        ctx->running = false;
        return WS_ERR_UNKNOWN;
    }

    ctx->thread_running = true;
    return WS_OK;
}

void ws_client_stop_thread(ws_client_context_t* ctx) {
    if (!ctx || !ctx->thread_running) return;

    ctx->running = false;
    ctx->should_reconnect = false;

    // Wait for thread to finish
    pthread_join(ctx->thread, NULL);
    ctx->thread_running = false;
}

// ============================================================================
// Messaging (Thread-safe through ws_client_t's mutex)
// ============================================================================

ws_error_t ws_client_send_text(ws_client_context_t* ctx, const char* text) {
    if (!ctx || !text) return WS_ERR_INVALID_PARAMETER;

    ws_error_t err = ws_send_text(&ctx->ws_client, text);
    if (err == WS_OK) {
        ctx->messages_sent++;
    }
    return err;
}

ws_error_t ws_client_send_binary(ws_client_context_t* ctx, const uint8_t* data, size_t len) {
    if (!ctx || !data) return WS_ERR_INVALID_PARAMETER;

    ws_error_t err = ws_send_binary(&ctx->ws_client, data, len);
    if (err == WS_OK) {
        ctx->messages_sent++;
    }
    return err;
}

ws_error_t ws_client_send_ping(ws_client_context_t* ctx) {
    if (!ctx) return WS_ERR_INVALID_PARAMETER;

    time_t now = get_current_time();
    ctx->last_ping_sent = now;

    return ws_send_ping(&ctx->ws_client, NULL, 0);
}

// ============================================================================
// Status & Info
// ============================================================================

bool ws_client_is_connected(ws_client_context_t* ctx) {
    if (!ctx) return false;
    return ctx->ws_client.state == WS_STATE_OPEN;
}

void ws_client_get_stats(ws_client_context_t* ctx, ws_statistics_t* stats) {
    if (!ctx || !stats) return;
    ws_get_statistics(&ctx->ws_client, stats);
}

uint64_t ws_client_get_reconnect_count(ws_client_context_t* ctx) {
    if (!ctx) return 0;
    return ctx->reconnect_count;
}
