# WebSocket Library Tests

This directory contains tests for the `libws` library.

## Echo Test (Local)

A simple echo test that verifies basic WebSocket functionality (text and binary messages).

### Running the Test

1. Build the project:
   ```bash
   make
   ```

2. Compile the test client:
   ```bash
   gcc -Wall -Wextra -g -pthread -D_GNU_SOURCE -I../include test_echo.c -o test_echo -L../build/lib -lwebsocket -lssl -lcrypto -pthread
   ```

3. Run the echo server in one terminal:
   ```bash
   ../build/bin/echo_server
   ```

4. Run the test client in another terminal:
   ```bash
   ./test_echo
   ```

## Autobahn Test Suite (Docker)

To verify RFC 6455 compliance, we use the [Autobahn Testsuite](https://github.com/crossbario/autobahn-testsuite).

### Prerequisites

- Docker installed and running.

### Running the Tests

1. Ensure the project is built (`make`).
2. Run the test script:
   ```bash
   ./run_autobahn.sh
   ```

This script will:
1. Start the `echo_server` locally.
2. Run the Autobahn test suite in a Docker container against the local server.
3. Generate reports in `tests/autobahn/reports/`.
4. Stop the `echo_server`.

### Configuration

The Autobahn configuration is located in `tests/autobahn/config/fuzzingclient.json`.
It is configured to connect to `ws://127.0.0.1:9001` (where `echo_server` runs).
