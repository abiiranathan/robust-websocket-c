# WebSocket C Library

A robust, event-driven WebSocket library written in C.

## Features

- **Event-Driven**: Callbacks for open, message, close, and error events.
- **Protocol Compliant**: Handles masking, control frames (Ping/Pong), and fragmentation.
- **Robust Buffering**: Automatically reassembles fragmented frames.
- **Easy API**: Simple functions to connect, accept, and send data.
- **Dependency Free**: Only depends on standard libraries and OpenSSL (for SHA1 and Base64).

## Project Structure

```
.
├── Makefile            # Build system
├── include
│   └── websocket.h     # Public API header
├── src
│   └── websocket.c     # Library implementation
├── examples
│   ├── client.c        # Example client
│   └── server.c        # Example chat server
└── build               # Build artifacts (created by make)
```

## Building

To build the library and examples:

```bash
make
```

This will create:
- `build/lib/libwebsocket.a`: The static library.
- `build/bin/server`: The example server.
- `build/bin/client`: The example client.

## Usage

### Server Example

Run the server:
```bash
./build/bin/server
```
The server listens on port 8080.

### Client Example

Run the client:
```bash
./build/bin/client
```

## API Overview

### Initialization

```c
ws_client_t client;
ws_init(&client);

client.on_open = on_open_callback;
client.on_message = on_message_callback;
client.on_close = on_close_callback;
client.on_error = on_error_callback;
```

### Connection (Client)

```c
ws_connect(&client, "localhost", 8080, "/");
```

### Sending Messages

```c
ws_send_text(&client, "Hello World");
ws_send_ping(&client); // Keep-alive
```

### Event Loop Integration

Call `ws_consume()` whenever data is available on the socket:

```c
// In your event loop (e.g., after read())
ws_consume(&client, buffer, bytes_read);
```

## Dependencies

- OpenSSL (`libssl-dev`) for SHA1 and Base64 operations during the handshake.

## License

MIT
