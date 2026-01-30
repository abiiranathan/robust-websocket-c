#ifndef WS_CLIENT_LIB_H
#define WS_CLIENT_LIB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "websocket.h"

// Client configuration options
typedef struct {
    // Connection settings
    const char* host;
    int port;
    const char* path;
    bool use_ssl;

    // Timeouts (in seconds, 0 = no timeout)
    int connect_timeout;
    int read_timeout;
    int ping_interval;  // How often to send pings (0 = disabled)
    int pong_timeout;   // How long to wait for pong response

    // Reconnection settings
    bool auto_reconnect;
    int max_reconnect_attempts;  // 0 = infinite
    int reconnect_delay_ms;      // Initial delay, doubles on each attempt

    // Advanced settings
    bool non_blocking;  // Use non-blocking I/O
    size_t read_buffer_size;
} ws_client_config_t;

// Client run modes
typedef enum {
    WS_RUN_MODE_BLOCKING,      // Block in ws_client_run() until disconnect
    WS_RUN_MODE_NON_BLOCKING,  // Return immediately, user calls ws_client_poll()
    WS_RUN_MODE_THREADED       // Run in separate thread
} ws_run_mode_t;

// Extended client context (wraps ws_client_t)
typedef struct {
    ws_client_t ws_client;
    ws_client_config_t config;

    // Runtime state
    volatile bool running;
    volatile bool should_reconnect;
    int reconnect_attempts;
    time_t last_ping_sent;
    time_t last_pong_received;
    time_t connection_started;

    // Thread support (if using WS_RUN_MODE_THREADED)
    pthread_t thread;
    bool thread_running;

    // Callbacks
    ws_on_pong_cb on_pong;

    // Statistics
    uint64_t reconnect_count;
    uint64_t messages_sent;
    uint64_t messages_received;
} ws_client_context_t;

// ============================================================================
// Core API
// ============================================================================

// Initialize client context with default configuration
void ws_client_init(ws_client_context_t* ctx);

// Configure client (call before connect)
void ws_client_configure(ws_client_context_t* ctx, ws_client_config_t* config);

// Connect to WebSocket server
ws_error_t ws_client_connect(ws_client_context_t* ctx);

// Disconnect from server
void ws_client_disconnect(ws_client_context_t* ctx);

// Cleanup and free resources
void ws_client_cleanup(ws_client_context_t* ctx);

// ============================================================================
// Run Modes
// ============================================================================

// Blocking: Run event loop until disconnect (returns when connection closes)
void ws_client_run(ws_client_context_t* ctx);

// Non-blocking: Process pending events with timeout (ms), returns immediately
// Returns: number of events processed, -1 on error
int ws_client_poll(ws_client_context_t* ctx, int timeout_ms);

// Threaded: Start client in background thread
ws_error_t ws_client_start_thread(ws_client_context_t* ctx);

// Stop threaded client and join thread
void ws_client_stop_thread(ws_client_context_t* ctx);

// ============================================================================
// Messaging (Thread-safe)
// ============================================================================

// Send text message
ws_error_t ws_client_send_text(ws_client_context_t* ctx, const char* text);

// Send binary message
ws_error_t ws_client_send_binary(ws_client_context_t* ctx, const uint8_t* data, size_t len);

// Send ping
ws_error_t ws_client_send_ping(ws_client_context_t* ctx);

// ============================================================================
// Status & Info
// ============================================================================

// Check if connected
bool ws_client_is_connected(ws_client_context_t* ctx);

// Get connection statistics
void ws_client_get_stats(ws_client_context_t* ctx, ws_statistics_t* stats);

// Get reconnection count
uint64_t ws_client_get_reconnect_count(ws_client_context_t* ctx);

// ============================================================================
// Utilities
// ============================================================================

// Default configuration
ws_client_config_t ws_client_default_config(void);

// Set default timeouts based on use case

// Gaming, trading
void ws_client_config_set_timeouts_realtime(ws_client_config_t* config);

// Chat, notifications
void ws_client_config_set_timeouts_standard(ws_client_config_t* config);

// File sync, monitoring
void ws_client_config_set_timeouts_relaxed(ws_client_config_t* config);

#endif /* WS_CLIENT_LIB_H */
