#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <pthread.h>
#include <stdbool.h>
#include "websocket.h"

// Forward declarations
typedef struct ws_server_s ws_server_t;

// Server events
typedef void (*ws_server_on_open_cb)(ws_client_t* client);
typedef void (*ws_server_on_message_cb)(ws_client_t* client, const uint8_t* msg, size_t size, int type);
typedef void (*ws_server_on_close_cb)(ws_client_t* client, int code, const char* reason);

// Configuration for the server
typedef struct {
    const char* host;
    int port;
    int thread_count;  // Number of worker threads (0 = auto-detect cores)
    int timeout_ms;    // Epoll timeout (default 100)

    // Callbacks
    ws_server_on_open_cb on_open;
    ws_server_on_message_cb on_message;
    ws_server_on_close_cb on_close;
} ws_server_config_t;

// Create a new server instance
ws_server_t* ws_server_create(ws_server_config_t* config);

// Start the server (blocks until stopped)
void ws_server_start(ws_server_t* server);

// Stop the server (can be called from signal handler or another thread)
void ws_server_stop(ws_server_t* server);

// Destroy the server and free resources
void ws_server_destroy(ws_server_t* server);

// Broadcast a text message to all connected clients
void ws_server_broadcast_text(ws_server_t* server, const char* msg);

// Broadcast binary data to all connected clients
void ws_server_broadcast_binary(ws_server_t* server, const uint8_t* data, size_t len);

// Filter callback for broadcast
// Returns true if message should be sent to this client
typedef bool (*ws_filter_cb_t)(ws_client_t* client, void* arg);

// Broadcast a text message to clients matching the filter
void ws_server_broadcast_text_filter(ws_server_t* server, const char* msg, ws_filter_cb_t filter, void* arg);

// Broadcast binary data to clients matching the filter
void ws_server_broadcast_binary_filter(ws_server_t* server, const uint8_t* data, size_t len, ws_filter_cb_t filter,
                                       void* arg);

// Get the total number of connected clients
size_t ws_server_get_client_count(ws_server_t* server);

#endif  // WS_SERVER_H
