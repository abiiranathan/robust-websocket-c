#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WebSocket opcodes
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT         0x1
#define WS_OPCODE_BINARY       0x2
#define WS_OPCODE_CLOSE        0x8
#define WS_OPCODE_PING         0x9
#define WS_OPCODE_PONG         0xA

// WebSocket status codes
#define WS_STATUS_NORMAL         1000
#define WS_STATUS_GOING_AWAY     1001
#define WS_STATUS_PROTOCOL_ERROR 1002
#define WS_STATUS_UNSUPPORTED    1003
#define WS_STATUS_NO_STATUS      1005
#define WS_STATUS_ABNORMAL       1006
#define WS_STATUS_INVALID_DATA   1007
#define WS_STATUS_POLICY         1008
#define WS_STATUS_TOO_LARGE      1009
#define WS_STATUS_EXTENSION      1010
#define WS_STATUS_UNEXPECTED     1011

// WebSocket frame structure
typedef struct {
    bool fin;
    uint8_t opcode;
    bool mask;
    uint64_t payload_length;
    uint8_t masking_key[4];
    uint8_t* payload;
} websocket_frame_t;

// WebSocket state
typedef enum { WS_STATE_CONNECTING, WS_STATE_OPEN, WS_STATE_CLOSING, WS_STATE_CLOSED } ws_state_t;

// Forward declaration
typedef struct ws_client_s ws_client_t;

// Callbacks
typedef void (*ws_on_open_cb)(ws_client_t* client);
typedef void (*ws_on_message_cb)(ws_client_t* client, const uint8_t* data, size_t size, int type);
typedef void (*ws_on_close_cb)(ws_client_t* client, int code, const char* reason);
typedef void (*ws_on_error_cb)(ws_client_t* client, const char* error);

// WebSocket client context
struct ws_client_s {
    int socket_fd;
    ws_state_t state;
    bool is_server;

    // Buffering for incoming data
    uint8_t* recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_len;

    // User data
    void* user_data;

    // Callbacks
    ws_on_open_cb on_open;
    ws_on_message_cb on_message;
    ws_on_close_cb on_close;
    ws_on_error_cb on_error;

    // Configuration
    size_t max_payload_size;

    // I/O Interface
    int (*write_cb)(struct ws_client_s* client, const uint8_t* data, size_t len);

    // Internal State for Buffering/Fragmentation
    uint8_t* send_buffer;
    size_t send_buffer_len;
    size_t send_buffer_cap;

    uint8_t* frag_buffer;  // Buffer for reassembling fragments
    size_t frag_buffer_len;
    uint8_t frag_opcode;  // Opcode of the first fragment
};

// ============================================================================
// Core Library Functions
// ============================================================================

// Initialize a WebSocket client structure
void ws_init(ws_client_t* client);

// Set a custom write callback (e.g., for SSL or non-blocking handling)
// Default is a blocking 'write()' to socket_fd
void ws_set_write_cb(ws_client_t* client, int (*cb)(ws_client_t* client, const uint8_t* data, size_t len));

// Process outgoing data (flush send buffer)
// Call this when the socket is writable (EPOLLOUT)
void ws_io_send(ws_client_t* client);

// Clean up WebSocket client resources
void ws_cleanup(ws_client_t* client);

// Connect to a WebSocket server
// Returns 0 on success, -1 on failure
int ws_connect(ws_client_t* client, const char* host, int port, const char* path);

// Accept a client connection (for server use)
// Performs the handshake. Returns 0 on success.
int ws_accept(ws_client_t* client, int client_fd);

// Process incoming data from the socket.
// Call this when the socket is readable.
// Returns 0 on success, -1 if the connection should be closed.
int ws_consume(ws_client_t* client, const uint8_t* data, size_t len);

// Send a text message
int ws_send_text(ws_client_t* client, const char* text);

// Send a binary message
int ws_send_binary(ws_client_t* client, const uint8_t* data, size_t len);

// Send a Ping frame (for keep-alive)
int ws_send_ping(ws_client_t* client);

// Close the connection
int ws_close(ws_client_t* client, int code, const char* reason);

// ============================================================================
// Low-level Utilities (exposed for advanced usage)
// ============================================================================

char* base64_encode(const unsigned char* input, int length);
char* generate_websocket_accept(const char* websocket_key);
char* extract_websocket_key(const char* request);
uint8_t* create_websocket_frame(websocket_frame_t* frame, size_t* out_length);

#endif /* WEBSOCKET_H */
