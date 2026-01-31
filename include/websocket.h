#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// WebSocket opcodes
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT         0x1
#define WS_OPCODE_BINARY       0x2
#define WS_OPCODE_CLOSE        0x8
#define WS_OPCODE_PING         0x9
#define WS_OPCODE_PONG         0xA

// WebSocket status codes (RFC 6455)
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

// Library Error Codes
typedef enum {
    WS_OK = 0,
    WS_ERR_ALLOCATION_FAILURE,
    WS_ERR_INVALID_PARAMETER,
    WS_ERR_INVALID_STATE,
    WS_ERR_DNS_LOOKUP_FAILED,
    WS_ERR_SOCKET_FAILED,
    WS_ERR_CONNECT_FAILED,
    WS_ERR_HANDSHAKE_FAILED,
    WS_ERR_PROTOCOL_VIOLATION,
    WS_ERR_PAYLOAD_TOO_LARGE,
    WS_ERR_INVALID_UTF8,
    WS_ERR_IO_ERROR,
    WS_ERR_LOCKED,

    // TLS/SSL Errors
    WS_ERR_SSL_FAILED,
    WS_ERR_CERT_VALIDATION_FAILED,
    WS_ERR_UNKNOWN
} ws_error_t;

// WebSocket frame structure
typedef struct {
    bool fin;
    bool rsv1;
    bool rsv2;
    bool rsv3;
    uint8_t opcode;
    bool mask;
    uint64_t payload_length;
    uint8_t masking_key[4];
    uint8_t* payload;
} websocket_frame_t;

// WebSocket state
typedef enum { WS_STATE_CLOSED, WS_STATE_CONNECTING, WS_STATE_OPEN, WS_STATE_CLOSING } ws_state_t;

// Statistics
typedef struct {
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t frames_received;
    uint64_t frames_sent;
    uint64_t pings_received;
    uint64_t pongs_received;
    uint64_t pings_sent;
    uint64_t errors_count;
    time_t connected_at;
    time_t closed_at;
} ws_statistics_t;

// Forward declaration
typedef struct ws_client_s ws_client_t;

// Callbacks
typedef void (*ws_on_open_cb)(ws_client_t* client);
typedef void (*ws_on_message_cb)(ws_client_t* client, const uint8_t* data, size_t size, int type);
typedef void (*ws_on_close_cb)(ws_client_t* client, int code, const char* reason);
typedef void (*ws_on_error_cb)(ws_client_t* client, const char* error);
typedef void (*ws_on_pong_cb)(ws_client_t* client, const uint8_t* data, size_t len);

// Custom I/O Callbacks
typedef int (*ws_write_cb_t)(ws_client_t* client, const uint8_t* data, size_t len);
typedef int (*ws_read_cb_t)(ws_client_t* client, uint8_t* buffer, size_t len);

// WebSocket client context
struct ws_client_s {
    int socket_fd;
    ws_state_t state;
    bool is_server;

    // SSL/TLS
    SSL* ssl;
    SSL_CTX* ssl_ctx;
    bool use_ssl;

    // Thread safety
    pthread_mutex_t lock;

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
    ws_on_pong_cb on_pong;

    // Configuration
    size_t max_payload_size;
    bool auto_fragment;
    size_t fragment_size;
    bool auto_ping;
    bool validate_utf8;

    // I/O Interface
    ws_write_cb_t write_cb;
    ws_read_cb_t read_cb;

    // Internal State for Buffering/Fragmentation
    uint8_t* send_buffer;  // For future non-blocking write buffering
    size_t send_buffer_len;
    size_t send_buffer_cap;

    // Fragmentation reassembly
    struct {
        uint8_t* buffer;
        size_t len;
        size_t cap;
    } frag_arena;

    uint8_t frag_opcode;  // Opcode of the first fragment
    bool in_fragmentation;

    // Statistics & Monitoring
    ws_statistics_t stats;
    struct timespec last_ping_at;
    struct timespec last_pong_at;
};

// ============================================================================
// Core Library Functions
// ============================================================================

// Initialize a WebSocket client structure
void ws_init(ws_client_t* client);

// Clean up WebSocket client resources
void ws_cleanup(ws_client_t* client);

// Connect to a WebSocket server
// Returns WS_OK on success, or error code
ws_error_t ws_connect(ws_client_t* client, const char* host, int port, const char* path);

// Accept a client connection (for server use)
// Performs the handshake.
ws_error_t ws_accept(ws_client_t* client, int client_fd);

// Process incoming data from the socket.
// Call this when the socket is readable.
ws_error_t ws_consume(ws_client_t* client, const uint8_t* data, size_t len);

// Read data from the underlying connection (SSL or socket).
// Returns bytes read, 0 on EOF, -1 on error.
ssize_t ws_read(ws_client_t* client, void* buffer, size_t len);

// Send a text message
ws_error_t ws_send_text(ws_client_t* client, const char* text);

// Send a text message with explicit length
ws_error_t ws_send_text_len(ws_client_t* client, const char* text, size_t len);

// Send a binary message
ws_error_t ws_send_binary(ws_client_t* client, const uint8_t* data, size_t len);

// Send a Ping frame
ws_error_t ws_send_ping(ws_client_t* client, const uint8_t* data, size_t len);

// Close the connection
ws_error_t ws_close(ws_client_t* client, int code, const char* reason);

// Check if connection is alive (based on state and last activity)
bool ws_is_alive(ws_client_t* client);

// Get human readable string for error code
const char* ws_strerror(ws_error_t err);

// ============================================================================
// Configuration
// ============================================================================

void ws_set_max_payload_size(ws_client_t* client, size_t size);
void ws_set_auto_fragment(ws_client_t* client, bool enable, size_t fragment_size);
void ws_set_auto_ping(ws_client_t* client,
                      bool enable);  // Note: library responds to pings automatically, this is for sending pings? Or
                                     // enabling auto-pong? Usually auto-pong is mandatory.
void ws_set_validate_utf8(ws_client_t* client, bool enable);
void ws_set_ssl(ws_client_t* client, bool enable);
void ws_set_write_cb(ws_client_t* client, ws_write_cb_t cb);
void ws_set_read_cb(ws_client_t* client, ws_read_cb_t cb);
void ws_set_user_data(ws_client_t* client, void* user_data);
void* ws_get_user_data(ws_client_t* client);
ws_state_t ws_get_state(ws_client_t* client);
void ws_get_statistics(ws_client_t* client, ws_statistics_t* stats);

// ============================================================================
// Interactive Mode (stdin + WebSocket)
// ============================================================================

// Typedef for line handler callback - called when a complete line is read from stdin
// client: WebSocket client (can be used to send messages)
// line: null-terminated string (without newline)
// user_data: optional user data passed to ws_run_interactive
typedef void (*ws_stdin_line_handler_t)(ws_client_t* client, const char* line, void* user_data);

// Run interactive client loop that reads from stdin and communicates with server
// This function:
//   - Reads from stdin using select() to avoid blocking on socket
//   - Calls handler for each complete line entered
//   - Processes incoming WebSocket messages via client callbacks
//   - Continues until connection closes
//
// Parameters:
//   client: Connected WebSocket client
//   handler: Callback function called for each complete line read
//   user_data: optional data passed to handler (can be NULL)
//   timeout_ms: Timeout for select() (0 for blocking, >0 for polling)
//
// Returns: WS_OK on normal disconnect, error code on failure
ws_error_t ws_run_interactive(ws_client_t* client, ws_stdin_line_handler_t handler, void* user_data, int timeout_ms);

// ============================================================================
// Low-level Utilities (exposed for testing/advanced usage)
// ============================================================================

char* base64_encode(const unsigned char* input, int length);
char* generate_websocket_accept(const char* websocket_key);
char* extract_websocket_key(const char* request);
// Returns number of bytes consumed or -1 on error
int parse_websocket_frame(const uint8_t* buffer, size_t len, websocket_frame_t* frame);

#endif /* WEBSOCKET_H */
