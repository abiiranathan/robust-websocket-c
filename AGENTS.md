# libws Agent Guidelines

This document provides guidelines for AI agents operating within the `libws` codebase. Adhering to these guidelines ensures consistency, maintainability, and alignment with project standards.

## 1. Build, Lint, and Test Commands

### 1.1 Build Commands

The project uses a `Makefile` for building.

*   **Full Build:** Compiles the `libwebsocket.a` static library and example executables (`chat_client`, `chat_server`, `echo_server`).
    ```bash
    make all
    ```

*   **Clean Build Artifacts:** Removes the `build/` directory and all its contents.
    ```bash
    make clean
    ```

### 1.2 Linting and Formatting

There are no explicit linting or automated formatting tools configured for this project. Agents should infer and adhere to the existing code style, primarily found in `src/` and `include/` directories.

### 1.3 Test Commands

The project includes integration tests and a comprehensive Autobahn test suite.

*   **Run `test_echo` Client:** This executable connects to a running WebSocket echo server (e.g., `build/bin/echo_server`) and performs basic text and binary echo tests.
    ```bash
    # First, ensure the echo server is running in a separate terminal or background:
    ./build/bin/echo_server &
    # Then, run the test client:
    ./build/bin/test_echo
    ```
    *Note: To run a single test within `test_echo.c` (if it were structured with multiple isolated tests), modifications to `test_echo.c` would be required to selectively execute test functions. Currently, it runs as a single sequence.*

*   **Run Autobahn Test Suite:** This script starts the `echo_server`, runs the Autobahn test suite via Docker, and then stops the server. It's the most comprehensive test available.
    ```bash
    ./tests/run_autobahn.sh
    ```
    *Prerequisite: Docker must be installed and running.*

## 2. Code Style Guidelines

This section outlines the preferred coding style and conventions for `libws`.

### 2.1 Imports and Includes

*   Standard C library includes should be placed first, followed by project-specific headers.
*   Internal project headers should use relative paths: `#include "../include/header.h"`

### 2.2 Formatting

*   **Indentation:** 4 spaces for indentation. No tabs.
*   **Bracing:**
    *   **Functions:**
        ```c
        void my_function() {
            // ...
        }
        ```
    *   **Control Structures (if, for, while, switch):** Allman style (opening brace on the same line).
        ```c
        if (condition) {
            // ...
        }
        ```
*   **Spacing:**
    *   Spaces around binary operators (`=`, `+`, `-`, `==`, `&&`, etc.).
    *   No space before `(`, but space after keywords like `if`, `for`, `while`.
    *   No spaces inside parentheses, brackets, or braces unless they contain expressions that benefit from it.
*   **Line Length:** Aim for a maximum of 120 characters per line, but prioritize readability over strict adherence for complex expressions or strings.
*   **Newlines:** Use newlines to separate logical blocks of code for improved readability.

### 2.3 Types

*   Use standard C types (`int`, `char*`, `size_t`, `uint8_t`, `bool`, `ssize_t`, `time_t`).
*   Where appropriate, use standard integer types in <stdint.h>
*   Utilize custom `_t` suffixed structs (e.g., `ws_client_t`, `websocket_frame_t`).

### 2.4 Naming Conventions

*   **Functions:** `snake_case`. Public API functions are prefixed with `ws_` (e.g., `ws_init`, `ws_connect`). Internal static functions also follow `snake_case`.
*   **Variables:** `snake_case` (e.g., `client_fd`, `payload_len`, `magic_string`).
*   **Macros:** `UPPER_SNAKE_CASE` (e.g., `MIN`, `BUFFER_GROWTH_CHUNK`). Specific WebSocket opcodes and states are prefixed with `WS_` (e.g., `WS_OPCODE_TEXT`, `WS_STATE_OPEN`).
*   **Enums:** `UPPER_SNAKE_CASE` with `WS_` prefix for values (e.g., `WS_ERR_ALLOCATION_FAILURE`, `WS_STATUS_NORMAL`).
*   **Structs:** `snake_case` with `_t` suffix (e.g., `ws_client_t`, `websocket_frame_t`).

### 2.5 Error Handling

*   Functions should return `ws_error_t` or `int` (0 for success, non-zero for error) to indicate operation status.
*   Perform null checks for pointers after memory allocation (`malloc`, `realloc`) and for function parameters.
*   Use `fprintf(stderr, ...)` for logging critical errors.
*   Ensure proper resource cleanup (e.g., `free`, `close`, `SSL_free`, `SSL_CTX_free`, `pthread_mutex_destroy`) on error paths.
*   Utilize `pthread_mutex_lock` and `pthread_mutex_unlock` for thread safety, with `pthread_mutex_trylock` in cleanup routines to prevent deadlocks.
*   Handle all `ws_error_t` return values appropriately.

### 2.6 Memory Management

*   Explicitly manage memory using `malloc`, `realloc`, and `free`.
*   Always check the return value of `malloc` and `realloc` for `NULL`.
*   Set pointers to `NULL` immediately after `free`ing the memory to prevent use-after-free bugs.

### 2.7 Comments

*   Use block comments for major sections of code (e.g., `// ============================`).
*   Use `//` for inline comments to explain complex logic or non-obvious code.
*   Avoid redundant comments that simply restate what the code does. Focus on *why* a particular approach was taken.
*   `// TODO:` comments can be used for identified future work or temporary solutions.

### 2.8 General Best Practices

*   **Modularity:** Keep functions small and focused on a single task.
*   **Consistency:** Maintain consistent style and practices throughout the codebase.
*   **Readability:** Write clear, concise, and understandable code.
*   **Security:** Be mindful of potential security vulnerabilities (e.g., buffer overflows, unvalidated inputs) and implement robust checks.
