#include "../include/websocket.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_GROWTH_CHUNK 1024

// Internal prototypes
static int handle_handshake_response(ws_client_t* client);
static int handle_handshake_request(ws_client_t* client);
static void dispatch_frame(ws_client_t* client, websocket_frame_t* frame);
static int send_raw(ws_client_t* client, const void* data, size_t len);

// ============================================================================
// Utility Functions
// ============================================================================

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

char* generate_websocket_key() {
    unsigned char random_bytes[16];
    srand(time(NULL));
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = rand() % 256;
    }
    return base64_encode(random_bytes, 16);
}

char* extract_websocket_key(const char* request) {
    const char* key_header = "Sec-WebSocket-Key:";
    char* start = strcasestr((char*)request, key_header);
    if (!start) return NULL;

    start += strlen(key_header);
    while (*start == ' ') start++;  // skip spaces

    char* end = strpbrk(start, "\r\n");
    if (!end) return NULL;

    long length = end - start;
    char* key = malloc((size_t)length + 1);
    strncpy(key, start, (size_t)length);
    key[(size_t)length] = '\0';
    return key;
}

// ============================================================================
// Core Library Implementation
// ============================================================================

void ws_init(ws_client_t* client) {
    memset(client, 0, sizeof(ws_client_t));
    client->socket_fd = -1;
    client->state = WS_STATE_CLOSED;
    client->recv_buffer = NULL;
    client->recv_buffer_size = 0;
    client->recv_buffer_len = 0;
    client->max_payload_size = 1024 * 1024 * 16;  // 16MB default limit
}

void ws_cleanup(ws_client_t* client) {
    if (client->recv_buffer) {
        free(client->recv_buffer);
        client->recv_buffer = NULL;
    }
    if (client->socket_fd != -1) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }
    client->state = WS_STATE_CLOSED;
}

int ws_connect(ws_client_t* client, const char* host, int port, const char* path) {
    struct hostent* server;
    struct sockaddr_in server_addr;

    server = gethostbyname(host);
    if (server == NULL) {
        if (client->on_error) client->on_error(client, "Could not resolve hostname");
        return -1;
    }

    client->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->socket_fd < 0) {
        if (client->on_error) client->on_error(client, "Error opening socket");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, (size_t)server->h_length);
    server_addr.sin_port = htons(port);

    if (connect(client->socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        if (client->on_error) client->on_error(client, "Error connecting to server");
        close(client->socket_fd);
        client->socket_fd = -1;
        return -1;
    }

    client->is_server = false;
    client->state = WS_STATE_CONNECTING;

    // Send Handshake
    char* key = generate_websocket_key();
    char request[1024];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             path, host, port, key);

    send_raw(client, request, strlen(request));
    free(key);

    return 0;
}

int ws_accept(ws_client_t* client, int client_fd) {
    client->socket_fd = client_fd;
    client->is_server = true;
    client->state = WS_STATE_CONNECTING;
    // We expect the first data to be the handshake request
    return 0;
}

int ws_consume(ws_client_t* client, const uint8_t* data, size_t len) {
    // Append data to buffer
    if (client->recv_buffer_len + len > client->recv_buffer_size) {
        size_t new_size = client->recv_buffer_len + len + BUFFER_GROWTH_CHUNK;
        uint8_t* new_buf = realloc(client->recv_buffer, new_size);
        if (!new_buf) {
            if (client->on_error) client->on_error(client, "Out of memory");
            return -1;
        }
        client->recv_buffer = new_buf;
        client->recv_buffer_size = new_size;
    }
    memcpy(client->recv_buffer + client->recv_buffer_len, data, len);
    client->recv_buffer_len += len;

    // State machine
    if (client->state == WS_STATE_CONNECTING) {
        if (client->is_server) {
            return handle_handshake_request(client);
        } else {
            return handle_handshake_response(client);
        }
    } else if (client->state == WS_STATE_OPEN) {
        // Parse frames
        while (client->recv_buffer_len > 0) {
            websocket_frame_t frame = {0};

            // Check headers
            if (client->recv_buffer_len < 2) break;  // Need more data

            // Basic parsing to determine length
            size_t header_len = 2;
            uint8_t* buf = client->recv_buffer;

            bool mask = (buf[1] & 0x80) != 0;
            uint64_t payload_len = buf[1] & 0x7F;

            if (payload_len == 126) {
                if (client->recv_buffer_len < 4) break;
                payload_len = ((uint64_t)buf[2] << 8) | buf[3];
                header_len += 2;
            } else if (payload_len == 127) {
                if (client->recv_buffer_len < 10) break;
                payload_len = 0;
                for (int i = 0; i < 8; i++) {
                    payload_len = (payload_len << 8) | buf[2 + i];
                }
                header_len += 8;
            }

            if (mask) header_len += 4;

            // Check max payload size to prevent DoS
            if (payload_len > client->max_payload_size) {
                if (client->on_error) client->on_error(client, "Frame too large");
                ws_close(client, WS_STATUS_TOO_LARGE, "Frame too large");
                return -1;
            }

            // Check if we have the full frame
            if (client->recv_buffer_len < header_len + payload_len) {
                break;  // Wait for more data
            }

            // Parse frame completely
            frame.fin = (buf[0] & 0x80) != 0;
            frame.opcode = buf[0] & 0x0F;
            frame.mask = mask;
            frame.payload_length = payload_len;

            size_t mask_offset = header_len - (mask ? 4 : 0);
            if (mask) {
                memcpy(frame.masking_key, buf + mask_offset, 4);
            }

            frame.payload = buf + header_len;  // Point to payload in buffer

            // Unmask if needed
            if (frame.mask) {
                for (uint64_t i = 0; i < frame.payload_length; i++) {
                    frame.payload[i] ^= frame.masking_key[i % 4];
                }
            }

            // Dispatch
            dispatch_frame(client, &frame);

            // Remove processed frame from buffer
            size_t total_frame_len = header_len + payload_len;
            size_t remaining = client->recv_buffer_len - total_frame_len;
            if (remaining > 0) {
                memmove(client->recv_buffer, client->recv_buffer + total_frame_len, remaining);
            }
            client->recv_buffer_len = remaining;
        }
    }

    return 0;
}

// ============================================================================
// Internal Helpers
// ============================================================================

static int send_raw(ws_client_t* client, const void* data, size_t len) {
    if (client->socket_fd < 0) return -1;
    ssize_t written = write(client->socket_fd, data, len);
    if (written != (ssize_t)len) return -1;
    return 0;
}

static int handle_handshake_response(ws_client_t* client) {
    // Look for double CRLF
    char* end = strstr((char*)client->recv_buffer, "\r\n\r\n");
    if (!end) return 0;  // Not full response yet

    *end = '\0';  // Temporarily terminate

    // Check status code
    if (strstr((char*)client->recv_buffer, "HTTP/1.1 101") == NULL) {
        if (client->on_error) client->on_error(client, "Invalid handshake response");
        return -1;
    }

    // TODO: Verify Sec-WebSocket-Accept

    // Move past headers
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
    // Look for double CRLF
    char* end = strstr((char*)client->recv_buffer, "\r\n\r\n");
    if (!end) return 0;

    char* key = extract_websocket_key((char*)client->recv_buffer);
    if (!key) {
        if (client->on_error) client->on_error(client, "No Sec-WebSocket-Key found");
        return -1;
    }

    char* accept_key = generate_websocket_accept(key);
    char response[1024];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_key);

    send_raw(client, response, strlen(response));

    free(key);
    free(accept_key);

    // Move past headers
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

static void dispatch_frame(ws_client_t* client, websocket_frame_t* frame) {
    switch (frame->opcode) {
        case WS_OPCODE_TEXT:
        case WS_OPCODE_BINARY:
            if (client->on_message) {
                client->on_message(client, frame->payload, (size_t)frame->payload_length, frame->opcode);
            }
            break;
        case WS_OPCODE_PING:
            // Auto-reply PONG
            {
                websocket_frame_t pong = *frame;
                pong.opcode = WS_OPCODE_PONG;
                pong.mask = !client->is_server;  // Client must mask, Server must not
                // Reuse payload from Ping
                size_t len;
                uint8_t* data = create_websocket_frame(&pong, &len);
                send_raw(client, data, len);
                free(data);
            }
            break;
        case WS_OPCODE_CLOSE:
            client->state = WS_STATE_CLOSING;
            if (client->on_close) {
                // Parse status code if present
                int code = WS_STATUS_NO_STATUS;
                if (frame->payload_length >= 2) {
                    code = (frame->payload[0] << 8) | frame->payload[1];
                }
                client->on_close(client, code, "Closed by peer");
            }
            // Echo close if we haven't sent one?
            // For now, just close.
            client->state = WS_STATE_CLOSED;
            break;
    }
}

uint8_t* create_websocket_frame(websocket_frame_t* frame, size_t* out_length) {
    size_t header_size = 2;
    if (frame->payload_length >= 126 && frame->payload_length <= 65535) {
        header_size += 2;
    } else if (frame->payload_length > 65535) {
        header_size += 8;
    }

    if (frame->mask) {
        header_size += 4;
    }

    *out_length = header_size + (size_t)frame->payload_length;
    uint8_t* buffer = (uint8_t*)malloc(*out_length);
    if (!buffer) return NULL;

    buffer[0] = (frame->fin ? 0x80 : 0x00) | (frame->opcode & 0x0F);

    if (frame->payload_length < 126) {
        buffer[1] = (frame->mask ? 0x80 : 0x00) | (frame->payload_length & 0x7F);
    } else if (frame->payload_length <= 65535) {
        buffer[1] = (frame->mask ? 0x80 : 0x00) | 126;
        buffer[2] = (frame->payload_length >> 8) & 0xFF;
        buffer[3] = frame->payload_length & 0xFF;
    } else {
        buffer[1] = (frame->mask ? 0x80 : 0x00) | 127;
        for (int i = 0; i < 8; i++) {
            buffer[2 + i] = (frame->payload_length >> (8 * (7 - i))) & 0xFF;
        }
    }

    size_t mask_offset = header_size - (frame->mask ? 4 : 0);
    if (frame->mask) {
        memcpy(buffer + mask_offset, frame->masking_key, 4);
    }

    memcpy(buffer + header_size, frame->payload, (size_t)frame->payload_length);

    if (frame->mask) {
        for (uint64_t i = 0; i < frame->payload_length; i++) {
            buffer[header_size + i] ^= frame->masking_key[i % 4];
        }
    }

    return buffer;
}

int ws_send_binary_internal(ws_client_t* client, const uint8_t* data, size_t len, int opcode) {
    websocket_frame_t frame;
    frame.fin = true;
    frame.opcode = opcode;
    frame.mask = !client->is_server;
    frame.payload_length = len;
    frame.payload = (uint8_t*)data;  // Cast away const, but we won't modify unless masking

    if (frame.mask) {
        // Generate random key
        for (int i = 0; i < 4; i++) frame.masking_key[i] = rand() % 256;
    }

    size_t out_len;
    uint8_t* out = create_websocket_frame(&frame, &out_len);
    if (!out) return -1;

    int res = send_raw(client, out, out_len);
    free(out);
    return res;
}

int ws_send_text(ws_client_t* client, const char* text) {
    return ws_send_binary_internal(client, (const uint8_t*)text, strlen(text), WS_OPCODE_TEXT);
}

int ws_send_binary(ws_client_t* client, const uint8_t* data, size_t len) {
    return ws_send_binary_internal(client, data, len, WS_OPCODE_BINARY);
}

int ws_close(ws_client_t* client, int code, const char* reason) {
    websocket_frame_t frame;
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
    frame.payload = payload;

    if (frame.mask) {
        for (int i = 0; i < 4; i++) frame.masking_key[i] = rand() % 256;
    }

    size_t out_len;
    uint8_t* out = create_websocket_frame(&frame, &out_len);
    if (out) {
        send_raw(client, out, out_len);
        free(out);
    }

    client->state = WS_STATE_CLOSING;
    return 0;
}
