# WebSocket C Library

A robust, thread-safe, and feature-rich WebSocket (RFC 6455) implementation in C.

## Features

- **Thread Safety**: Full thread safety with mutex protection for all public APIs.
- **RFC 6455 Compliant**: Passes Autobahn test suite requirements (fragmentation, masking, UTF-8, control frames).
- **Comprehensive Error Handling**: Structured error codes (`WS_ERR_*`) and human-readable strings via `ws_strerror()`.
- **Fragmentation Support**: Automatic message fragmentation and reassembly.
- **UTF-8 Validation**: Strict RFC 3629 UTF-8 validation for text frames and close reasons.
- **Security**: 
  - Uses `/dev/urandom` for masking keys.
  - Configurable payload limits to prevent DoS.
  - Proper state machine enforcement.
- **Monitoring**: Built-in statistics (frames sent/received, bytes, ping/pong tracking).
- **Flexible IO**: Custom read/write callbacks supported (easy integration with SSL/TLS or event loops).

## Project Structure

```
.
├── Makefile            # Build system
├── include
│   └── websocket.h     # Public API header
├── src
│   └── websocket.c     # Library implementation
├── examples
│   ├── client.c        # Simple client
│   ├── server.c        # Non-blocking epoll server
│   ├── chat_client.c   # Interactive chat client
│   └── chat_server.c   # Chat server
└── build               # Build artifacts
```

## Building

To build the library and examples:

```bash
make
```

Dependencies:
- OpenSSL (`libssl-dev`)
- Pthread (`-pthread`)

## API Overview

### Initialization & Configuration

```c
#include "websocket.h"

ws_client_t client;
ws_init(&client);

// Set callbacks
client.on_open = on_open;
client.on_message = on_message;
client.on_close = on_close;
client.on_error = on_error;

// Configuration
ws_set_max_payload_size(&client, 1024 * 1024); // 1MB limit
ws_set_auto_fragment(&client, true, 4096);     // Fragment outgoing > 4KB
ws_set_validate_utf8(&client, true);           // Enforce UTF-8 checking
```

### Connection (Client Mode)

```c
ws_error_t err = ws_connect(&client, "localhost", 8080, "/");
if (err != WS_OK) {
    fprintf(stderr, "Connect failed: %s\n", ws_strerror(err));
}
```

### Server Side (Handshake)

```c
// After accepting a TCP connection
ws_accept(&client, client_fd);
```

### Sending Messages

```c
// Send Text
ws_send_text(&client, "Hello World");

// Send Binary
uint8_t data[] = {0x01, 0x02, 0x03};
ws_send_binary(&client, data, sizeof(data));

// Send Ping (Pong is handled automatically)
ws_send_ping(&client, NULL, 0);
```

### Event Loop Integration

Feed data from your socket into the library:

```c
// In your read loop
ssize_t n = read(socket_fd, buffer, sizeof(buffer));
if (n > 0) {
    ws_consume(&client, buffer, (size_t)n);
}
```

### Thread Safety

The library uses `pthread_mutex` internally. You can safely call `ws_send_*` or `ws_close` from multiple threads.

### Statistics

```c
ws_statistics_t stats;
ws_get_statistics(&client, &stats);
printf("Bytes Received: %lu\n", stats.bytes_received);
```

## Error Codes

The library uses `ws_error_t` for return values:

- `WS_OK`
- `WS_ERR_ALLOCATION_FAILURE`
- `WS_ERR_INVALID_PARAMETER`
- `WS_ERR_CONNECT_FAILED`
- `WS_ERR_PROTOCOL_VIOLATION`
- `WS_ERR_PAYLOAD_TOO_LARGE`
- ... and more.

Use `ws_strerror(err)` to get the string representation.

## License

MIT
