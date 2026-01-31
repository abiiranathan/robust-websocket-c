#include "../include/websocket.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define BUFFER_GROWTH_CHUNK 4096

// ============================================================================
// Internal Prototypes
// ============================================================================

static int handle_handshake_response(ws_client_t* client);
static int handle_handshake_request(ws_client_t* client);
static void dispatch_frame(ws_client_t* client, websocket_frame_t* frame);
static int send_raw_data(ws_client_t* client, const void* data, size_t len);
static ws_error_t send_fragmented(ws_client_t* client, const uint8_t* data, size_t len, int opcode);
static bool is_valid_utf8(const uint8_t* data, size_t len);
static void ws_get_random_bytes(void* buf, size_t len);
static void reset_fragment_buffer(ws_client_t* client);

// ============================================================================
// Utilities
// ============================================================================

const char* ws_strerror(ws_error_t err) {
    switch (err) {
        case WS_OK:
            return "Success";
        case WS_ERR_ALLOCATION_FAILURE:
            return "Memory allocation failure";
        case WS_ERR_INVALID_PARAMETER:
            return "Invalid parameter";
        case WS_ERR_INVALID_STATE:
            return "Invalid state";
        case WS_ERR_DNS_LOOKUP_FAILED:
            return "DNS lookup failed";
        case WS_ERR_SOCKET_FAILED:
            return "Socket creation failed";
        case WS_ERR_CONNECT_FAILED:
            return "Connection failed";
        case WS_ERR_HANDSHAKE_FAILED:
            return "Handshake failed";
        case WS_ERR_PROTOCOL_VIOLATION:
            return "Protocol violation";
        case WS_ERR_PAYLOAD_TOO_LARGE:
            return "Payload too large";
        case WS_ERR_INVALID_UTF8:
            return "Invalid UTF-8 sequence";
        case WS_ERR_IO_ERROR:
            return "I/O error";
        case WS_ERR_LOCKED:
            return "Resource locked";
        case WS_ERR_SSL_FAILED:
            return "SSL/TLS error";
        case WS_ERR_CERT_VALIDATION_FAILED:
            return "Certificate validation failed";
        case WS_ERR_UNKNOWN:
        default:
            return "Unknown error";
    }
}

static void ws_get_random_bytes(void* buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t bytes_read = read(fd, buf, len);
        close(fd);
        if (bytes_read == (ssize_t)len) {
            return;
        }
    }
    // Fallback (less secure but functional)
    uint8_t* p = (uint8_t*)buf;
    for (size_t i = 0; i < len; ++i) {
        p[i] = rand() % 256;
    }
}

// RFC 3629 compliant UTF-8 validation
static bool is_valid_utf8(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (data[i] <= 0x7F) {  // 1-byte sequence (0xxxxxxx)
            i += 1;
        } else if ((data[i] & 0xE0) == 0xC0) {  // 2-byte sequence (110xxxxx 10xxxxxx)
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80) return false;
            if (data[i] < 0xC2) return false;  // Overlong
            i += 2;
        } else if ((data[i] & 0xF0) == 0xE0) {  // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
            if (i + 2 >= len || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) return false;
            if (data[i] == 0xE0 && data[i + 1] < 0xA0) return false;   // Overlong
            if (data[i] == 0xED && data[i + 1] >= 0xA0) return false;  // Surrogate
            i += 3;
        } else if ((data[i] & 0xF8) == 0xF0) {  // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            if (i + 3 >= len || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80)
                return false;
            if (data[i] == 0xF0 && data[i + 1] < 0x90) return false;   // Overlong
            if (data[i] == 0xF4 && data[i + 1] >= 0x90) return false;  // > U+10FFFF
            if (data[i] > 0xF4) return false;                          // > U+10FFFF
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

static char* base64_encode(const unsigned char* input, int length) {
    if (!input || length <= 0) return NULL;

    BIO *bio, *b64;
    BUF_MEM* bufferPtr;

    b64 = BIO_new(BIO_f_base64());
    if (!b64) return NULL;

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_new(BIO_s_mem());
    if (!bio) {
        BIO_free(b64);
        return NULL;
    }
    bio = BIO_push(b64, bio);

    BIO_write(bio, input, length);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);

    char* result = (char*)malloc(bufferPtr->length + 1);
    if (!result) {
        BIO_free_all(bio);
        return NULL;
    }
    memcpy(result, bufferPtr->data, bufferPtr->length);
    result[bufferPtr->length] = 0;

    BIO_free_all(bio);
    return result;
}

static char* generate_websocket_accept(const char* websocket_key) {
    if (!websocket_key) return NULL;

    const char* magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", websocket_key, magic_string);

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), hash);

    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

static char* extract_websocket_key(const char* request) {
    if (!request) return NULL;

    const char* key_header = "Sec-WebSocket-Key:";
    char* start = strcasestr((char*)request, key_header);
    if (!start) return NULL;

    start += strlen(key_header);
    while (*start == ' ') start++;

    char* end = strpbrk(start, "\r\n");
    if (!end) return NULL;

    long length = end - start;
    if (length <= 0) return NULL;

    char* key = malloc((size_t)length + 1);
    if (!key) return NULL;
    strncpy(key, start, (size_t)length);
    key[(size_t)length] = '\0';
    return key;
}

// Helper to safely reset fragment buffer (Arena style - retains memory)
static void reset_fragment_buffer(ws_client_t* client) {
    // Reset length only, do not free memory (Arena reset)
    client->frag_arena.len = 0;

    client->in_fragmentation = false;
    client->frag_opcode = 0;
}

// ============================================================================
// Core
// ============================================================================

void ws_init(ws_client_t* client) {
    if (!client) return;

    memset(client, 0, sizeof(ws_client_t));
    client->socket_fd = -1;
    client->state = WS_STATE_CLOSED;
    client->ssl = NULL;
    client->ssl_ctx = NULL;
    client->use_ssl = false;

    // Default config
    client->max_payload_size = 1024 * 1024 * 64;  // 64 MB
    client->validate_utf8 = true;
    client->auto_fragment = false;
    client->fragment_size = 4096;
    client->auto_ping = true;
    client->close_code = 0;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&client->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    srand(time(NULL));
}

void ws_cleanup(ws_client_t* client) {
    if (!client) return;

    // Try to acquire lock, but proceed anyway to cleanup if it fails
    // (in case of deadlock during cleanup)
    int lock_result = pthread_mutex_trylock(&client->lock);
    bool locked = (lock_result == 0);

    // Mark as closed immediately to prevent new operations
    client->state = WS_STATE_CLOSED;

    // SSL cleanup
    if (client->ssl) {
        SSL_shutdown(client->ssl);
        SSL_free(client->ssl);
        client->ssl = NULL;
    }

    if (client->ssl_ctx) {
        SSL_CTX_free(client->ssl_ctx);
        client->ssl_ctx = NULL;
    }

    // Free buffers with NULL checks and NULL assignment
    if (client->recv_buffer) {
        free(client->recv_buffer);
        client->recv_buffer = NULL;
    }
    client->recv_buffer_size = 0;
    client->recv_buffer_len = 0;

    if (client->send_buffer) {
        free(client->send_buffer);
        client->send_buffer = NULL;
    }
    client->send_buffer_len = 0;
    client->send_buffer_cap = 0;

    if (client->frag_arena.buffer) {
        free(client->frag_arena.buffer);
        client->frag_arena.buffer = NULL;
    }
    client->frag_arena.len = 0;
    client->frag_arena.cap = 0;

    // Socket cleanup
    if (client->socket_fd != -1) {
        shutdown(client->socket_fd, SHUT_RDWR);
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    if (locked) {
        pthread_mutex_unlock(&client->lock);
    }

    // Destroy mutex last
    pthread_mutex_destroy(&client->lock);
}

static int default_read_cb(ws_client_t* client, uint8_t* buffer, size_t len) {
    if (!client || !buffer || len == 0) return -1;
    if (client->socket_fd < 0) return -1;

    if (client->use_ssl && client->ssl) {
        int r = SSL_read(client->ssl, buffer, (int)len);
        if (r <= 0) {
            int err = SSL_get_error(client->ssl, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
                return -1;
            }
        }
        return r;
    }

    return read(client->socket_fd, buffer, len);
}

static int default_write_cb(ws_client_t* client, const uint8_t* data, size_t len) {
    if (!client || !data || len == 0) return -1;
    if (client->socket_fd < 0) return -1;

    size_t sent = 0;
    while (sent < len) {
        if (client->use_ssl && client->ssl) {
            int chunk = (len - sent) > 2147483647 ? 2147483647 : (int)(len - sent);
            int written = SSL_write(client->ssl, data + sent, chunk);
            if (written <= 0) {
                int err = SSL_get_error(client->ssl, written);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                     // For blocking sockets, this shouldn't typically happen unless
                     // explicitly configured. We'll treat it as a retry.
                     continue; 
                }
                return -1;
            }
            sent += (size_t)written;
            client->stats.bytes_sent += (uint64_t)written;
        } else {
            ssize_t written = send(client->socket_fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            sent += (size_t)written;
            client->stats.bytes_sent += (uint64_t)written;
        }
    }
    return 0;
}

static int send_raw_data(ws_client_t* client, const void* data, size_t len) {
    if (!client || !data || len == 0) return -1;

    if (client->write_cb) {
        return client->write_cb(client, (const uint8_t*)data, len);
    }
    return default_write_cb(client, (const uint8_t*)data, len);
}

ws_error_t ws_connect(ws_client_t* client, const char* host, int port, const char* path) {
    if (!client || !host || !path) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);

    // Auto-detect SSL port
    if (port == 443) client->use_ssl = true;

    if (client->state != WS_STATE_CLOSED) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_INVALID_STATE;
    }

    struct hostent* server = gethostbyname(host);
    if (server == NULL) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_DNS_LOOKUP_FAILED;
    }

    client->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->socket_fd < 0) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_SOCKET_FAILED;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, (size_t)server->h_length);
    server_addr.sin_port = htons(port);

    if (connect(client->socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_CONNECT_FAILED;
    }

    if (client->use_ssl) {
        client->ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!client->ssl_ctx) {
            close(client->socket_fd);
            client->socket_fd = -1;
            pthread_mutex_unlock(&client->lock);
            return WS_ERR_ALLOCATION_FAILURE;
        }

        // Load default trust store
        if (SSL_CTX_set_default_verify_paths(client->ssl_ctx) != 1) {
            SSL_CTX_free(client->ssl_ctx);
            client->ssl_ctx = NULL;
            close(client->socket_fd);
            client->socket_fd = -1;
            pthread_mutex_unlock(&client->lock);
            return WS_ERR_CERT_VALIDATION_FAILED;
        }

        SSL_CTX_set_verify(client->ssl_ctx, SSL_VERIFY_PEER, NULL);

        client->ssl = SSL_new(client->ssl_ctx);
        if (!client->ssl) {
            SSL_CTX_free(client->ssl_ctx);
            client->ssl_ctx = NULL;
            close(client->socket_fd);
            client->socket_fd = -1;
            pthread_mutex_unlock(&client->lock);
            return WS_ERR_SSL_FAILED;
        }

        SSL_set_fd(client->ssl, client->socket_fd);

        // SNI
        SSL_set_tlsext_host_name(client->ssl, host);

        if (SSL_connect(client->ssl) != 1) {
            SSL_free(client->ssl);
            client->ssl = NULL;
            SSL_CTX_free(client->ssl_ctx);
            client->ssl_ctx = NULL;
            close(client->socket_fd);
            client->socket_fd = -1;
            pthread_mutex_unlock(&client->lock);
            return WS_ERR_SSL_FAILED;
        }

        // Verify Certificate
        if (SSL_get_verify_result(client->ssl) != X509_V_OK) {
            SSL_free(client->ssl);
            client->ssl = NULL;
            SSL_CTX_free(client->ssl_ctx);
            client->ssl_ctx = NULL;
            close(client->socket_fd);
            client->socket_fd = -1;
            pthread_mutex_unlock(&client->lock);
            return WS_ERR_CERT_VALIDATION_FAILED;
        }
    }

    client->is_server = false;
    client->state = WS_STATE_CONNECTING;
    client->stats.connected_at = time(NULL);

    // Generate Key
    unsigned char random_bytes[16];
    ws_get_random_bytes(random_bytes, 16);
    char* key = base64_encode(random_bytes, 16);
    if (!key) {
        if (client->ssl) {
            SSL_free(client->ssl);
            client->ssl = NULL;
        }
        if (client->ssl_ctx) {
            SSL_CTX_free(client->ssl_ctx);
            client->ssl_ctx = NULL;
        }
        close(client->socket_fd);
        client->socket_fd = -1;
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_ALLOCATION_FAILURE;
    }

    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             path, host, port, key);

    int send_result = send_raw_data(client, request, strlen(request));
    free(key);

    if (send_result < 0) {
        if (client->ssl) {
            SSL_free(client->ssl);
            client->ssl = NULL;
        }
        if (client->ssl_ctx) {
            SSL_CTX_free(client->ssl_ctx);
            client->ssl_ctx = NULL;
        }
        close(client->socket_fd);
        client->socket_fd = -1;
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_IO_ERROR;
    }

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

ws_error_t ws_accept(ws_client_t* client, int client_fd) {
    if (!client || client_fd < 0) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);

    if (client->state != WS_STATE_CLOSED) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_INVALID_STATE;
    }

    client->socket_fd = client_fd;
    client->is_server = true;
    client->state = WS_STATE_CONNECTING;
    client->stats.connected_at = time(NULL);

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

static int handle_handshake_response(ws_client_t* client) {
    char* end = strstr((char*)client->recv_buffer, "\r\n\r\n");
    if (!end) return 0;  // Incomplete

    *end = '\0';

    // Basic status check
    if (strstr((char*)client->recv_buffer, "HTTP/1.1 101") == NULL) {
        if (client->on_error) {
            client->on_error(client, "Handshake failed: Invalid Status");
        }
        return -1;
    }

    // Move buffer
    size_t header_len = (size_t)(end - (char*)client->recv_buffer) + 4;
    size_t remaining = client->recv_buffer_len - header_len;
    if (remaining > 0) {
        memmove(client->recv_buffer, client->recv_buffer + header_len, remaining);
    }
    client->recv_buffer_len = remaining;

    client->state = WS_STATE_OPEN;
    if (client->on_open) {
        client->on_open(client);
    }

    return 0;
}

static int handle_handshake_request(ws_client_t* client) {
    char* end = strstr((char*)client->recv_buffer, "\r\n\r\n");
    if (!end) return 0;

    char* key = extract_websocket_key((char*)client->recv_buffer);
    if (!key) {
        if (client->on_error) {
            client->on_error(client, "Missing Sec-WebSocket-Key");
        }
        return -1;
    }

    char* accept_key = generate_websocket_accept(key);
    free(key);

    if (!accept_key) {
        if (client->on_error) {
            client->on_error(client, "Failed to generate accept key");
        }
        return -1;
    }

    char response[2048];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_key);

    int send_result = send_raw_data(client, response, strlen(response));
    free(accept_key);

    if (send_result < 0) {
        if (client->on_error) {
            client->on_error(client, "Failed to send handshake response");
        }
        return -1;
    }

    size_t header_len = (size_t)(end - (char*)client->recv_buffer) + 4;
    size_t remaining = client->recv_buffer_len - header_len;
    if (remaining > 0) {
        memmove(client->recv_buffer, client->recv_buffer + header_len, remaining);
    }
    client->recv_buffer_len = remaining;

    client->state = WS_STATE_OPEN;
    if (client->on_open) {
        client->on_open(client);
    }

    return 0;
}

// Returns bytes consumed or -1 on error/incomplete
static int parse_websocket_frame(const uint8_t* buffer, size_t len, websocket_frame_t* frame) {
    if (!buffer || !frame || len < 2) return 0;

    memset(frame, 0, sizeof(websocket_frame_t));

    frame->fin = (buffer[0] & 0x80) != 0;
    frame->rsv1 = (buffer[0] & 0x40) != 0;
    frame->rsv2 = (buffer[0] & 0x20) != 0;
    frame->rsv3 = (buffer[0] & 0x10) != 0;
    frame->opcode = buffer[0] & 0x0F;
    frame->mask = (buffer[1] & 0x80) != 0;

    uint64_t payload_len = buffer[1] & 0x7F;
    size_t header_len = 2;

    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = ((uint64_t)buffer[2] << 8) | buffer[3];
        header_len += 2;
    } else if (payload_len == 127) {
        if (len < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | buffer[2 + i];
        }
        header_len += 8;
    }

    if (frame->mask) header_len += 4;

    if (len < header_len + payload_len) return 0;  // Incomplete

    frame->payload_length = payload_len;

    if (frame->mask) {
        memcpy(frame->masking_key, buffer + header_len - 4, 4);
    }

    frame->payload = (uint8_t*)(buffer + header_len);

    return (int)(header_len + payload_len);
}

ws_error_t ws_consume(ws_client_t* client, const uint8_t* data, size_t len) {
    if (!client || !data || len == 0) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);

    // Grow buffer if needed
    if (client->recv_buffer_len + len > client->recv_buffer_size) {
        size_t new_size = client->recv_buffer_len + len + BUFFER_GROWTH_CHUNK;
        uint8_t* new_buf = realloc(client->recv_buffer, new_size);
        if (!new_buf) {
            pthread_mutex_unlock(&client->lock);
            return WS_ERR_ALLOCATION_FAILURE;
        }
        client->recv_buffer = new_buf;
        client->recv_buffer_size = new_size;
    }
    memcpy(client->recv_buffer + client->recv_buffer_len, data, len);
    client->recv_buffer_len += len;
    client->stats.bytes_received += len;

    if (client->state == WS_STATE_CONNECTING) {
        int res;
        if (client->is_server)
            res = handle_handshake_request(client);
        else
            res = handle_handshake_response(client);

        pthread_mutex_unlock(&client->lock);
        return (res == 0) ? WS_OK : WS_ERR_HANDSHAKE_FAILED;
    }

    if (client->state == WS_STATE_OPEN || client->state == WS_STATE_CLOSING) {
        while (client->recv_buffer_len > 0) {
            websocket_frame_t frame;
            int bytes_consumed = parse_websocket_frame(client->recv_buffer, client->recv_buffer_len, &frame);

            if (bytes_consumed == 0) break;  // Need more data
            if (bytes_consumed < 0) {
                pthread_mutex_unlock(&client->lock);
                return WS_ERR_PROTOCOL_VIOLATION;
            }

            // Protocol Validation
            if (frame.rsv1 || frame.rsv2 || frame.rsv3) {
                ws_close(client, WS_STATUS_PROTOCOL_ERROR, "RSV bits used");
                pthread_mutex_unlock(&client->lock);
                return WS_ERR_PROTOCOL_VIOLATION;
            }

            if (client->is_server && !frame.mask) {
                ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Client must mask");
                pthread_mutex_unlock(&client->lock);
                return WS_ERR_PROTOCOL_VIOLATION;
            }
            if (!client->is_server && frame.mask) {
                ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Server must not mask");
                pthread_mutex_unlock(&client->lock);
                return WS_ERR_PROTOCOL_VIOLATION;
            }

            // Unmask
            if (frame.mask) {
                for (uint64_t i = 0; i < frame.payload_length; i++) {
                    frame.payload[i] ^= frame.masking_key[i % 4];
                }
            }

            // Dispatch
            dispatch_frame(client, &frame);
            client->stats.frames_received++;

            // Remove processed frame
            size_t remaining = client->recv_buffer_len - (size_t)bytes_consumed;
            if (remaining > 0) {
                memmove(client->recv_buffer, client->recv_buffer + bytes_consumed, remaining);
            }
            client->recv_buffer_len = remaining;

            // Check if connection was closed during dispatch
            if (client->state == WS_STATE_CLOSING || client->state == WS_STATE_CLOSED) {
                // If closing/closed, we stop processing further frames
                pthread_mutex_unlock(&client->lock);
                return WS_OK;  // Or appropriate status
            }
        }
    }

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

ssize_t ws_read(ws_client_t* client, void* buffer, size_t len) {
    if (!client || !buffer) return -1;

    if (client->read_cb) {
        return client->read_cb(client, (uint8_t*)buffer, len);
    }
    return default_read_cb(client, (uint8_t*)buffer, len);
}

static void dispatch_frame(ws_client_t* client, websocket_frame_t* frame) {
    if (!client || !frame) return;

    // Control frames can appear in the middle of fragmented frames
    if (frame->opcode >= 0x8) {  // Control frame
        if (frame->payload_length > 125) {
            ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Control frame too large");
            return;
        }
        if (!frame->fin) {
            ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Control frame must not be fragmented");
            return;
        }

        switch (frame->opcode) {
            case WS_OPCODE_CLOSE: {
                int code = WS_STATUS_NORMAL;
                char reason[128] = {0};
                if (frame->payload_length >= 2) {
                    code = (frame->payload[0] << 8) | frame->payload[1];

                    // Validate close code
                    bool valid_code = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) ||
                                      (code >= 3000 && code <= 4999);

                    if (!valid_code) {
                        // Invalid close code
                        ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Invalid close code");
                        client->state = WS_STATE_CLOSED;
                        client->stats.closed_at = time(NULL);
                        if (client->on_close) {
                            client->on_close(client, WS_STATUS_PROTOCOL_ERROR, "Invalid close code");
                        }
                        return;
                    }

                    if (frame->payload_length > 2) {
                        size_t rlen = frame->payload_length - 2;
                        if (rlen > 123) rlen = 123;
                        memcpy(reason, frame->payload + 2, rlen);
                        // Validate UTF8 of reason
                        if (client->validate_utf8 && !is_valid_utf8((uint8_t*)reason, rlen)) {
                            code = WS_STATUS_INVALID_DATA;  // Fail the close logic?
                            // RFC6455 says: if invalid utf8 in reason, fail connection
                            ws_close(client, WS_STATUS_INVALID_DATA, "Invalid UTF-8 in close reason");
                            client->state = WS_STATE_CLOSED;
                            client->stats.closed_at = time(NULL);
                            if (client->on_close) {
                                client->on_close(client, WS_STATUS_INVALID_DATA, "Invalid UTF-8 in close reason");
                            }
                            return;
                        }
                    }
                } else if (frame->payload_length == 1) {
                    ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Invalid close payload length");
                    client->state = WS_STATE_CLOSED;
                    client->stats.closed_at = time(NULL);
                    if (client->on_close) {
                        client->on_close(client, WS_STATUS_PROTOCOL_ERROR, "Invalid close payload length");
                    }
                    return;
                }

                if (client->state == WS_STATE_OPEN) {
                    // Respond with close
                    ws_close(client, code, "Closed by peer");
                }
                client->state = WS_STATE_CLOSED;
                client->stats.closed_at = time(NULL);
                if (client->on_close) {
                    client->on_close(client, code, reason);
                }
                break;
            }
            case WS_OPCODE_PING:
                client->stats.pings_received++;
                clock_gettime(CLOCK_MONOTONIC, &client->last_ping_at);

                // Auto Pong
                {
                    uint8_t header[14];
                    size_t header_size = 2;
                    uint8_t masking_key[4] = {0};

                    header[0] = 0x80 | WS_OPCODE_PONG;
                    size_t payload_len = (size_t)frame->payload_length;

                    bool should_mask = !client->is_server;
                    if (payload_len < 126) {
                        header[1] = (should_mask ? 0x80 : 0x00) | payload_len;
                    }

                    if (should_mask) {
                        ws_get_random_bytes(masking_key, 4);
                        memcpy(header + 2, masking_key, 4);
                        header_size += 4;
                    }

                    // Send header
                    send_raw_data(client, header, header_size);

                    // Send payload (masked)
                    if (should_mask && payload_len > 0) {
                        uint8_t masked_payload[125];
                        memcpy(masked_payload, frame->payload, payload_len);
                        for (size_t i = 0; i < payload_len; ++i) {
                            masked_payload[i] ^= masking_key[i % 4];
                        }
                        send_raw_data(client, masked_payload, payload_len);
                    } else if (payload_len > 0) {
                        send_raw_data(client, frame->payload, payload_len);
                    }
                }
                break;
            case WS_OPCODE_PONG:
                client->stats.pongs_received++;
                clock_gettime(CLOCK_MONOTONIC, &client->last_pong_at);
                if (client->on_pong) {
                    client->on_pong(client, frame->payload, frame->payload_length);
                }
                break;
            default:
                ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Unknown control opcode");
                break;
        }
        return;
    }

    // Data Frames (Text/Binary) & Continuation
    if (frame->opcode != WS_OPCODE_CONTINUATION && frame->opcode != WS_OPCODE_TEXT &&
        frame->opcode != WS_OPCODE_BINARY) {
        ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Unknown opcode");
        return;
    }

    // Validate fragmentation state
    if (frame->opcode == WS_OPCODE_CONTINUATION && !client->in_fragmentation) {
        ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Unexpected continuation frame");
        return;
    }
    if (frame->opcode != WS_OPCODE_CONTINUATION && client->in_fragmentation) {
        ws_close(client, WS_STATUS_PROTOCOL_ERROR, "Expected continuation frame");
        return;
    }

    // Start of fragmentation or single frame
    if (frame->opcode != WS_OPCODE_CONTINUATION) {
        client->frag_opcode = frame->opcode;
        client->in_fragmentation = !frame->fin;
    } else {
        // Continuation
        if (frame->fin) {
            client->in_fragmentation = false;
        }
    }

    // Buffer handling
    size_t needed_len = client->frag_arena.len + frame->payload_length;
    if (needed_len > client->max_payload_size) {
        reset_fragment_buffer(client);
        ws_close(client, WS_STATUS_TOO_LARGE, "Message too large");
        return;
    }

    // Append to fragmentation arena (grow if needed)
    if (needed_len > client->frag_arena.cap) {
        size_t new_cap = needed_len + BUFFER_GROWTH_CHUNK;
        uint8_t* new_buf = realloc(client->frag_arena.buffer, new_cap);
        if (!new_buf) {
            reset_fragment_buffer(client);
            if (client->on_error) {
                client->on_error(client, "OOM during reassembly");
            }
            return;
        }

        client->frag_arena.buffer = new_buf;
        client->frag_arena.cap = new_cap;
    }

    if (frame->payload_length > 0) {
        // Protect against memcpy with NULL buffer (though realloc should handle cap>0)
        // If cap was 0 and payload_length > 0, we entered the realloc block above.
        // If payload_length is 0, we don't copy.
        memcpy(client->frag_arena.buffer + client->frag_arena.len, frame->payload, frame->payload_length);
    }
    client->frag_arena.len += frame->payload_length;

    if (frame->fin) {
        // Message complete
        int type = client->frag_opcode;

        // UTF-8 Validation
        if (client->validate_utf8 && type == WS_OPCODE_TEXT) {
            if (!is_valid_utf8(client->frag_arena.buffer, client->frag_arena.len)) {
                reset_fragment_buffer(client);
                ws_close(client, WS_STATUS_INVALID_DATA, "Invalid UTF-8");
                return;
            }
        }

        if (client->on_message) {
            client->on_message(client, client->frag_arena.buffer, client->frag_arena.len, type);
        }

        // Reset buffer
        reset_fragment_buffer(client);
    }
}

// Internal function, expects lock to be held
static ws_error_t send_fragmented(ws_client_t* client, const uint8_t* data, size_t len, int opcode) {
    if (!client) return WS_ERR_INVALID_PARAMETER;
    if (len > 0 && !data) return WS_ERR_INVALID_PARAMETER;
    if (client->state != WS_STATE_OPEN) return WS_ERR_INVALID_STATE;

    size_t offset = 0;
    bool first = true;

    do {
        size_t chunk_size = len - offset;
        if (client->auto_fragment && chunk_size > client->fragment_size) {
            chunk_size = client->fragment_size;
        }

        // Ensure we send at least one frame if len is 0
        if (len == 0) {
            chunk_size = 0;
        }

        bool last = (offset + chunk_size >= len);

        websocket_frame_t frame = {0};
        frame.fin = last;
        frame.opcode = first ? opcode : WS_OPCODE_CONTINUATION;
        frame.mask = !client->is_server;
        frame.payload_length = chunk_size;

        uint8_t header[14];
        size_t header_size = 2;

        header[0] = (frame.fin ? 0x80 : 0x00) | (frame.opcode & 0x0F);

        if (chunk_size < 126) {
            header[1] = (frame.mask ? 0x80 : 0x00) | chunk_size;
        } else if (chunk_size <= 65535) {
            header[1] = (frame.mask ? 0x80 : 0x00) | 126;
            header[2] = (chunk_size >> 8) & 0xFF;
            header[3] = chunk_size & 0xFF;
            header_size += 2;
        } else {
            header[1] = (frame.mask ? 0x80 : 0x00) | 127;
            for (int i = 0; i < 8; i++) {
                header[2 + i] = (chunk_size >> (8 * (7 - i))) & 0xFF;
            }
            header_size += 8;
        }

        if (frame.mask) {
            ws_get_random_bytes(frame.masking_key, 4);
            memcpy(header + header_size, frame.masking_key, 4);
            header_size += 4;
        }

        if (send_raw_data(client, header, header_size) < 0) {
            return WS_ERR_IO_ERROR;
        }

        if (frame.mask && chunk_size > 0) {
            uint8_t* masked = malloc(chunk_size);
            if (!masked) return WS_ERR_ALLOCATION_FAILURE;

            const uint8_t* src = data + offset;
            for (size_t i = 0; i < chunk_size; ++i) {
                masked[i] = src[i] ^ frame.masking_key[i % 4];
            }
            int res = send_raw_data(client, masked, chunk_size);
            free(masked);
            if (res < 0) return WS_ERR_IO_ERROR;
        } else if (chunk_size > 0) {
            if (send_raw_data(client, data + offset, chunk_size) < 0) {
                return WS_ERR_IO_ERROR;
            }
        }

        client->stats.bytes_sent += chunk_size;
        client->stats.frames_sent++;

        offset += chunk_size;
        first = false;

        // If len is 0, we are done after one iteration
        if (len == 0) break;

    } while (offset < len);

    return WS_OK;
}

ws_error_t ws_send_text(ws_client_t* client, const char* text, size_t len) {
    if (!client) return WS_ERR_INVALID_PARAMETER;
    if (len > 0 && !text) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);
    ws_error_t err = send_fragmented(client, (const uint8_t*)text, len, WS_OPCODE_TEXT);
    pthread_mutex_unlock(&client->lock);
    return err;
}

ws_error_t ws_send_binary(ws_client_t* client, const uint8_t* data, size_t len) {
    if (!client) return WS_ERR_INVALID_PARAMETER;
    if (len > 0 && !data) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);
    ws_error_t err = send_fragmented(client, data, len, WS_OPCODE_BINARY);
    pthread_mutex_unlock(&client->lock);
    return err;
}

ws_error_t ws_send_ping(ws_client_t* client, const uint8_t* data, size_t len) {
    if (!client) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);

    if (len > 125) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_INVALID_PARAMETER;
    }

    // Pings are not fragmented
    websocket_frame_t frame = {0};
    frame.fin = true;
    frame.opcode = WS_OPCODE_PING;
    frame.mask = !client->is_server;
    frame.payload_length = len;

    uint8_t header[14];
    size_t header_size = 2;
    header[0] = 0x80 | WS_OPCODE_PING;
    header[1] = (frame.mask ? 0x80 : 0x00) | len;

    if (frame.mask) {
        ws_get_random_bytes(frame.masking_key, 4);
        memcpy(header + 2, frame.masking_key, 4);
        header_size += 4;
    }

    if (send_raw_data(client, header, header_size) < 0) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_IO_ERROR;
    }

    if (len > 0) {
        if (frame.mask) {
            uint8_t masked[125];
            for (size_t i = 0; i < len; ++i) {
                masked[i] = data[i] ^ frame.masking_key[i % 4];
            }
            if (send_raw_data(client, masked, len) < 0) {
                pthread_mutex_unlock(&client->lock);
                return WS_ERR_IO_ERROR;
            }
        } else {
            if (send_raw_data(client, data, len) < 0) {
                pthread_mutex_unlock(&client->lock);
                return WS_ERR_IO_ERROR;
            }
        }
    }

    client->stats.pings_sent++;

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

ws_error_t ws_close(ws_client_t* client, int code, const char* reason) {
    if (!client) return WS_ERR_INVALID_PARAMETER;

    pthread_mutex_lock(&client->lock);

    // Prevent double-close
    if (client->state == WS_STATE_CLOSED || client->state == WS_STATE_CLOSING) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_INVALID_STATE;
    }

    client->state = WS_STATE_CLOSING;
    client->close_code = (uint16_t)code;

    websocket_frame_t frame = {0};
    frame.fin = true;
    frame.opcode = WS_OPCODE_CLOSE;
    frame.mask = !client->is_server;

    uint8_t payload[128];
    payload[0] = (code >> 8) & 0xFF;
    payload[1] = code & 0xFF;
    size_t reason_len = reason ? strlen(reason) : 0;
    if (reason_len > 123) reason_len = 123;
    if (reason && reason_len > 0) {
        memcpy(payload + 2, reason, reason_len);
    }

    frame.payload_length = 2 + reason_len;

    uint8_t header[14];
    size_t header_size = 2;
    header[0] = 0x80 | WS_OPCODE_CLOSE;
    size_t len = (size_t)frame.payload_length;
    header[1] = (frame.mask ? 0x80 : 0x00) | len;

    if (frame.mask) {
        ws_get_random_bytes(frame.masking_key, 4);
        memcpy(header + 2, frame.masking_key, 4);
        header_size += 4;
    }

    if (send_raw_data(client, header, header_size) < 0) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_IO_ERROR;
    }

    if (frame.mask) {
        uint8_t masked[125];
        for (size_t i = 0; i < len; ++i) {
            masked[i] = payload[i] ^ frame.masking_key[i % 4];
        }
        send_raw_data(client, masked, len);
    } else {
        send_raw_data(client, payload, len);
    }

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

// ============================================================================
// Internal / Configuration
// ============================================================================

void ws_set_max_payload_size(ws_client_t* client, size_t size) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->max_payload_size = size;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_auto_fragment(ws_client_t* client, bool enable, size_t fragment_size) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->auto_fragment = enable;
    client->fragment_size = fragment_size;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_auto_ping(ws_client_t* client, bool enable) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->auto_ping = enable;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_validate_utf8(ws_client_t* client, bool enable) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->validate_utf8 = enable;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_ssl(ws_client_t* client, bool enable) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->use_ssl = enable;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_write_cb(ws_client_t* client, ws_write_cb_t cb) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->write_cb = cb;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_read_cb(ws_client_t* client, ws_read_cb_t cb) {
    if (!client) return;
    pthread_mutex_lock(&client->lock);
    client->read_cb = cb;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_user_data(ws_client_t* client, void* user_data) {
    if (!client) return;
    client->user_data = user_data;
}

void* ws_get_user_data(ws_client_t* client) { return client ? client->user_data : NULL; }

ws_state_t ws_get_state(ws_client_t* client) {
    if (!client) return WS_STATE_CLOSED;

    pthread_mutex_lock(&client->lock);
    ws_state_t s = client->state;
    pthread_mutex_unlock(&client->lock);
    return s;
}

void ws_get_statistics(ws_client_t* client, ws_statistics_t* stats) {
    if (!client || !stats) return;

    pthread_mutex_lock(&client->lock);
    *stats = client->stats;
    pthread_mutex_unlock(&client->lock);
}

bool ws_is_alive(ws_client_t* client) {
    if (!client) return false;

    pthread_mutex_lock(&client->lock);
    bool alive = (client->state == WS_STATE_OPEN);
    pthread_mutex_unlock(&client->lock);
    return alive;
}
// ============================================================================
// Interactive Mode (stdin + WebSocket)
// ============================================================================

#define WS_STDIN_BUFFER_SIZE 4096

typedef struct {
    uint8_t buffer[WS_STDIN_BUFFER_SIZE];
    size_t len;
} ws_stdin_reader_t;

ws_error_t ws_run_interactive(ws_client_t* client, ws_stdin_line_handler_t handler, void* user_data, int timeout_ms) {
    if (!client || !handler) {
        return WS_ERR_INVALID_PARAMETER;
    }

    ws_stdin_reader_t stdin_reader = {0};
    uint8_t ws_buffer[4096];
    int stdin_fd = STDIN_FILENO;

    // Continue while connected, connecting, or closing
    while (true) {
        ws_state_t state = ws_get_state(client);

        // Exit if connection is closed or failed
        if (state == WS_STATE_CLOSED) {
            break;
        }

        fd_set readfds;
        FD_ZERO(&readfds);

        if (stdin_fd != -1) {
            FD_SET(stdin_fd, &readfds);
        }
        FD_SET(client->socket_fd, &readfds);

        int max_fd = (client->socket_fd > stdin_fd) ? client->socket_fd : stdin_fd;

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return WS_ERR_IO_ERROR;
        }

        // Handle stdin (user input)
        if (stdin_fd != -1 && FD_ISSET(stdin_fd, &readfds)) {
            ssize_t n = read(stdin_fd, stdin_reader.buffer + stdin_reader.len,
                             sizeof(stdin_reader.buffer) - stdin_reader.len - 1);

            if (n > 0) {
                stdin_reader.len += (size_t)n;
                stdin_reader.buffer[stdin_reader.len] = '\0';

                // Process complete lines
                char* newline;
                while ((newline = (char*)memchr(stdin_reader.buffer, '\n', stdin_reader.len))) {
                    *newline = '\0';

                    // Remove carriage return if present
                    if (newline > (char*)stdin_reader.buffer && *(newline - 1) == '\r') {
                        *(newline - 1) = '\0';
                    }

                    // Only call handler if connected (not connecting or closing)
                    if (state == WS_STATE_OPEN) {
                        handler(client, (const char*)stdin_reader.buffer, user_data);
                    }

                    // Move remaining data to front
                    size_t line_len = (size_t)(newline - (char*)stdin_reader.buffer) + 1;
                    size_t remaining = stdin_reader.len - line_len;
                    if (remaining > 0) {
                        memmove(stdin_reader.buffer, stdin_reader.buffer + line_len, remaining);
                    }
                    stdin_reader.len = remaining;
                    stdin_reader.buffer[stdin_reader.len] = '\0';
                }

                // Protect against overly long input lines
                if (stdin_reader.len >= sizeof(stdin_reader.buffer) - 1) {
                    fprintf(stderr, "[Error] Input line too long\n");
                    stdin_reader.len = 0;
                    stdin_reader.buffer[0] = '\0';
                }
            } else if (n == 0) {
                // EOF on stdin
                stdin_fd = -1;
            } else {
                if (errno != EINTR && errno != EAGAIN) {
                    return WS_ERR_IO_ERROR;
                }
            }
        }

        // Handle data from server (needed for handshake and messages)
        if (FD_ISSET(client->socket_fd, &readfds)) {
            ssize_t n = ws_read(client, ws_buffer, sizeof(ws_buffer));

            if (n < 0) {
                if (errno != EINTR) {
                    return WS_ERR_IO_ERROR;
                }
            } else if (n == 0) {
                // Connection closed
                break;
            } else {
                ws_error_t err = ws_consume(client, ws_buffer, (size_t)n);
                if (err != WS_OK) {
                    return err;
                }
            }
        }
    }

    return WS_OK;
}