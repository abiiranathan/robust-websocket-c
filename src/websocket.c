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
        case WS_ERR_UNKNOWN:
        default:
            return "Unknown error";
    }
}

static void ws_get_random_bytes(void* buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, buf, len);
        close(fd);
    } else {
        // Fallback (less secure but functional)
        uint8_t* p = (uint8_t*)buf;
        for (size_t i = 0; i < len; ++i) {
            p[i] = rand() % 256;
        }
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

char* base64_encode(const unsigned char* input, int length) {
    BIO *bio, *b64;
    BUF_MEM* bufferPtr;

    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_new(BIO_s_mem());
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

char* generate_websocket_accept(const char* websocket_key) {
    const char* magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", websocket_key, magic_string);

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), hash);

    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

char* extract_websocket_key(const char* request) {
    const char* key_header = "Sec-WebSocket-Key:";
    char* start = strcasestr((char*)request, key_header);
    if (!start) return NULL;

    start += strlen(key_header);
    while (*start == ' ') start++;

    char* end = strpbrk(start, "\r\n");
    if (!end) return NULL;

    long length = end - start;
    char* key = malloc((size_t)length + 1);
    if (!key) return NULL;
    strncpy(key, start, (size_t)length);
    key[(size_t)length] = '\0';
    return key;
}

// ============================================================================
// Core
// ============================================================================

void ws_init(ws_client_t* client) {
    memset(client, 0, sizeof(ws_client_t));
    client->socket_fd = -1;
    client->state = WS_STATE_CLOSED;

    // Default config
    client->max_payload_size = 1024 * 1024 * 16;  // 16MB
    client->validate_utf8 = true;
    client->auto_fragment = false;
    client->fragment_size = 4096;
    client->auto_ping = true;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&client->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    srand(time(NULL));
}

void ws_cleanup(ws_client_t* client) {
    pthread_mutex_lock(&client->lock);

    if (client->recv_buffer) free(client->recv_buffer);
    if (client->send_buffer) free(client->send_buffer);
    if (client->frag_buffer) free(client->frag_buffer);

    if (client->socket_fd != -1) {
        // Shutdown socket first to stop traffic
        shutdown(client->socket_fd, SHUT_RDWR);
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    client->state = WS_STATE_CLOSED;

    pthread_mutex_unlock(&client->lock);
    pthread_mutex_destroy(&client->lock);
}

static int default_write_cb(ws_client_t* client, const uint8_t* data, size_t len) {
    if (client->socket_fd < 0) return -1;
    ssize_t written = send(client->socket_fd, data, len, MSG_NOSIGNAL);
    if (written != (ssize_t)len) return -1;
    client->stats.bytes_sent += len;
    return 0;
}

static int send_raw_data(ws_client_t* client, const void* data, size_t len) {
    if (client->write_cb) {
        return client->write_cb(client, (const uint8_t*)data, len);
    }
    return default_write_cb(client, (const uint8_t*)data, len);
}

ws_error_t ws_connect(ws_client_t* client, const char* host, int port, const char* path) {
    pthread_mutex_lock(&client->lock);

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

    client->is_server = false;
    client->state = WS_STATE_CONNECTING;
    client->stats.connected_at = time(NULL);

    // Generate Key
    unsigned char random_bytes[16];
    ws_get_random_bytes(random_bytes, 16);
    char* key = base64_encode(random_bytes, 16);

    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             path, host, port, key);

    send_raw_data(client, request, strlen(request));
    free(key);

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

ws_error_t ws_accept(ws_client_t* client, int client_fd) {
    pthread_mutex_lock(&client->lock);
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
        if (client->on_error) client->on_error(client, "Handshake failed: Invalid Status");
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
    if (client->on_open) client->on_open(client);

    return 0;
}

static int handle_handshake_request(ws_client_t* client) {
    char* end = strstr((char*)client->recv_buffer, "\r\n\r\n");
    if (!end) return 0;

    char* key = extract_websocket_key((char*)client->recv_buffer);
    if (!key) {
        if (client->on_error) client->on_error(client, "Missing Sec-WebSocket-Key");
        return -1;
    }

    char* accept_key = generate_websocket_accept(key);
    char response[2048];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_key);

    send_raw_data(client, response, strlen(response));

    free(key);
    free(accept_key);

    size_t header_len = (size_t)(end - (char*)client->recv_buffer) + 4;
    size_t remaining = client->recv_buffer_len - header_len;
    if (remaining > 0) {
        memmove(client->recv_buffer, client->recv_buffer + header_len, remaining);
    }
    client->recv_buffer_len = remaining;

    client->state = WS_STATE_OPEN;
    if (client->on_open) client->on_open(client);

    return 0;
}

// Returns bytes consumed or -1 on error/incomplete
int parse_websocket_frame(const uint8_t* buffer, size_t len, websocket_frame_t* frame) {
    if (len < 2) return 0;

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

    frame->payload = (uint8_t*)(buffer + header_len);  // Points into the buffer

    return header_len + payload_len;
}

ws_error_t ws_consume(ws_client_t* client, const uint8_t* data, size_t len) {
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

            // Protocol Validation
            if (frame.rsv1 || frame.rsv2 || frame.rsv3) {
                // Extensions not supported
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
        }
    }

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

static void dispatch_frame(ws_client_t* client, websocket_frame_t* frame) {
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
                    if (frame->payload_length > 2) {
                        size_t rlen = frame->payload_length - 2;
                        if (rlen > 123) rlen = 123;
                        memcpy(reason, frame->payload + 2, rlen);
                        // Validate UTF8 of reason
                        if (client->validate_utf8 && !is_valid_utf8((uint8_t*)reason, rlen)) {
                            code = WS_STATUS_INVALID_DATA;  // Update code for invalid utf8
                        }
                    }
                }

                if (client->state == WS_STATE_OPEN) {
                    // Respond with close
                    ws_close(client, code, "Closed by peer");
                }
                client->state = WS_STATE_CLOSED;
                client->stats.closed_at = time(NULL);
                if (client->on_close) client->on_close(client, code, reason);
                break;
            }
            case WS_OPCODE_PING:
                client->stats.pings_received++;
                clock_gettime(CLOCK_MONOTONIC, &client->last_ping_at);

                // Auto Pong
                {
                    websocket_frame_t pong = *frame;
                    pong.opcode = WS_OPCODE_PONG;
                    pong.mask = !client->is_server;
                    // Reuse payload (create_websocket_frame copies it)

                    // Create frame manually to avoid mutex recursion if we called public API
                    // But public API handles splitting/masking...
                    // Let's use internal send helper
                    uint8_t header[14];  // Max header
                    size_t header_size = 2;

                    header[0] = 0x80 | WS_OPCODE_PONG;
                    size_t payload_len = (size_t)frame->payload_length;

                    if (payload_len < 126) {
                        header[1] = (pong.mask ? 0x80 : 0x00) | payload_len;
                    }  // Control frames max 125, so no 16/64 bit lengths

                    if (pong.mask) {
                        ws_get_random_bytes(pong.masking_key, 4);
                        header_size += 4;
                        memcpy(header + 2, pong.masking_key, 4);
                    }

                    // Send header
                    send_raw_data(client, header, header_size);

                    // Send payload (masked)
                    if (pong.mask) {
                        uint8_t masked_payload[125];
                        memcpy(masked_payload, frame->payload, payload_len);
                        for (size_t i = 0; i < payload_len; ++i) masked_payload[i] ^= pong.masking_key[i % 4];
                        send_raw_data(client, masked_payload, payload_len);
                    } else {
                        send_raw_data(client, frame->payload, payload_len);
                    }
                }
                break;
            case WS_OPCODE_PONG:
                client->stats.pongs_received++;
                clock_gettime(CLOCK_MONOTONIC, &client->last_pong_at);
                if (client->on_pong) client->on_pong(client, frame->payload, frame->payload_length);
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
        if (frame->fin) client->in_fragmentation = false;
    }

    // Buffer handling
    if (client->frag_buffer_len + frame->payload_length > client->max_payload_size) {
        ws_close(client, WS_STATUS_TOO_LARGE, "Message too large");
        return;
    }

    // Append to fragmentation buffer
    uint8_t* new_frag = realloc(client->frag_buffer, client->frag_buffer_len + frame->payload_length);
    if (!new_frag) {
        if (client->on_error) client->on_error(client, "OOM during reassembly");
        return;
    }
    client->frag_buffer = new_frag;
    memcpy(client->frag_buffer + client->frag_buffer_len, frame->payload, frame->payload_length);
    client->frag_buffer_len += frame->payload_length;

    if (frame->fin) {
        // Message complete
        int type = client->frag_opcode;

        // UTF-8 Validation
        if (client->validate_utf8 && type == WS_OPCODE_TEXT) {
            if (!is_valid_utf8(client->frag_buffer, client->frag_buffer_len)) {
                ws_close(client, WS_STATUS_INVALID_DATA, "Invalid UTF-8");
                // Reset buffer
                free(client->frag_buffer);
                client->frag_buffer = NULL;
                client->frag_buffer_len = 0;
                return;
            }
        }

        if (client->on_message) {
            client->on_message(client, client->frag_buffer, client->frag_buffer_len, type);
        }

        // Reset buffer
        free(client->frag_buffer);
        client->frag_buffer = NULL;
        client->frag_buffer_len = 0;
    }
}

// Internal function, expects lock to be held
static ws_error_t send_fragmented(ws_client_t* client, const uint8_t* data, size_t len, int opcode) {
    if (client->state != WS_STATE_OPEN) return WS_ERR_INVALID_STATE;

    size_t offset = 0;
    bool first = true;

    while (offset < len) {
        size_t chunk_size = len - offset;
        if (client->auto_fragment && chunk_size > client->fragment_size) {
            chunk_size = client->fragment_size;
        }

        bool last = (offset + chunk_size >= len);

        websocket_frame_t frame = {0};
        frame.fin = last;
        frame.opcode = first ? opcode : WS_OPCODE_CONTINUATION;
        frame.mask = !client->is_server;
        frame.payload_length = chunk_size;

        // Payload handling
        // For sending, we just write the header then the payload directly to socket to avoid malloc
        // But we need to mask it if client.

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
            for (int i = 0; i < 8; i++) header[2 + i] = (chunk_size >> (8 * (7 - i))) & 0xFF;
            header_size += 8;
        }

        if (frame.mask) {
            ws_get_random_bytes(frame.masking_key, 4);
            memcpy(header + header_size, frame.masking_key, 4);
            header_size += 4;
        }

        if (send_raw_data(client, header, header_size) < 0) return WS_ERR_IO_ERROR;

        if (frame.mask) {
            // Need to mask payload. To avoid allocating a huge buffer for the whole chunk,
            // we can stream it or alloc small chunks.
            // Let's alloc the chunk, mask, send, free.
            uint8_t* masked = malloc(chunk_size);
            if (!masked) return WS_ERR_ALLOCATION_FAILURE;
            const uint8_t* src = data + offset;
            for (size_t i = 0; i < chunk_size; ++i) masked[i] = src[i] ^ frame.masking_key[i % 4];
            int res = send_raw_data(client, masked, chunk_size);
            free(masked);
            if (res < 0) return WS_ERR_IO_ERROR;
        } else {
            if (send_raw_data(client, data + offset, chunk_size) < 0) return WS_ERR_IO_ERROR;
        }

        client->stats.bytes_sent += chunk_size;
        client->stats.frames_sent++;

        offset += chunk_size;
        first = false;
    }
    return WS_OK;
}

ws_error_t ws_send_text(ws_client_t* client, const char* text) {
    pthread_mutex_lock(&client->lock);
    ws_error_t err = send_fragmented(client, (const uint8_t*)text, strlen(text), WS_OPCODE_TEXT);
    pthread_mutex_unlock(&client->lock);
    return err;
}

ws_error_t ws_send_binary(ws_client_t* client, const uint8_t* data, size_t len) {
    pthread_mutex_lock(&client->lock);
    ws_error_t err = send_fragmented(client, data, len, WS_OPCODE_BINARY);
    pthread_mutex_unlock(&client->lock);
    return err;
}

ws_error_t ws_send_ping(ws_client_t* client, const uint8_t* data, size_t len) {
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
            for (size_t i = 0; i < len; ++i) masked[i] = data[i] ^ frame.masking_key[i % 4];
            send_raw_data(client, masked, len);
        } else {
            send_raw_data(client, data, len);
        }
    }

    client->stats.pings_sent++;

    pthread_mutex_unlock(&client->lock);
    return WS_OK;
}

ws_error_t ws_close(ws_client_t* client, int code, const char* reason) {
    pthread_mutex_lock(&client->lock);

    if (client->state == WS_STATE_CLOSED) {
        pthread_mutex_unlock(&client->lock);
        return WS_ERR_INVALID_STATE;
    }

    client->state = WS_STATE_CLOSING;

    websocket_frame_t frame = {0};
    frame.fin = true;
    frame.opcode = WS_OPCODE_CLOSE;
    frame.mask = !client->is_server;

    uint8_t payload[128];
    payload[0] = (code >> 8) & 0xFF;
    payload[1] = code & 0xFF;
    size_t reason_len = reason ? strlen(reason) : 0;
    if (reason_len > 123) reason_len = 123;
    if (reason) memcpy(payload + 2, reason, reason_len);

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
        for (size_t i = 0; i < len; ++i) masked[i] = payload[i] ^ frame.masking_key[i % 4];
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
    pthread_mutex_lock(&client->lock);
    client->max_payload_size = size;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_auto_fragment(ws_client_t* client, bool enable, size_t fragment_size) {
    pthread_mutex_lock(&client->lock);
    client->auto_fragment = enable;
    client->fragment_size = fragment_size;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_auto_ping(ws_client_t* client, bool enable) {
    pthread_mutex_lock(&client->lock);
    client->auto_ping = enable;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_validate_utf8(ws_client_t* client, bool enable) {
    pthread_mutex_lock(&client->lock);
    client->validate_utf8 = enable;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_write_cb(ws_client_t* client, ws_write_cb_t cb) {
    pthread_mutex_lock(&client->lock);
    client->write_cb = cb;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_read_cb(ws_client_t* client, ws_read_cb_t cb) {
    pthread_mutex_lock(&client->lock);
    client->read_cb = cb;
    pthread_mutex_unlock(&client->lock);
}

void ws_set_user_data(ws_client_t* client, void* user_data) {
    client->user_data = user_data;  // Atomic pointer assignment usually
}

void* ws_get_user_data(ws_client_t* client) { return client->user_data; }

ws_state_t ws_get_state(ws_client_t* client) {
    // Technically should lock, but reading int is usually atomic enough for state checks
    // Locking for correctness
    pthread_mutex_lock(&client->lock);
    ws_state_t s = client->state;
    pthread_mutex_unlock(&client->lock);
    return s;
}

void ws_get_statistics(ws_client_t* client, ws_statistics_t* stats) {
    pthread_mutex_lock(&client->lock);
    *stats = client->stats;
    pthread_mutex_unlock(&client->lock);
}

bool ws_is_alive(ws_client_t* client) {
    pthread_mutex_lock(&client->lock);
    bool alive = (client->state == WS_STATE_OPEN);
    // Could check last_pong_at vs current time if auto-ping is enabled
    pthread_mutex_unlock(&client->lock);
    return alive;
}
