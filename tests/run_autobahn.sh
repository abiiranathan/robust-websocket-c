#!/bin/bash
set -e

# Path to the echo server binary
SERVER_BIN="./build/bin/echo_server"

# Configuration directory for Autobahn
CONFIG_DIR="$(pwd)/tests/autobahn/config"
REPORT_DIR="$(pwd)/tests/autobahn/reports"

# Ensure report directory exists
mkdir -p "$REPORT_DIR"

# Start the echo server
echo "Starting echo server..."
$SERVER_BIN &
SERVER_PID=$!

# Wait for server to start
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "Server failed to start."
    exit 1
fi

echo "Running Autobahn test suite..."

# Run the Autobahn docker container
# We use --network host so the container can access the server on localhost
if docker run --rm \
    --network host \
    -v "${CONFIG_DIR}:/config" \
    -v "${REPORT_DIR}:/reports" \
    crossbario/autobahn-testsuite \
    wstest -m fuzzingclient -s /config/fuzzingclient.json; then
    echo "Autobahn tests completed successfully."
else
    echo "Autobahn tests failed."
    # Don't exit immediately, we want to kill the server
fi

# Stop the server
echo "Stopping echo server (PID: $SERVER_PID)..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true

echo "Done. Reports generated in $REPORT_DIR"
