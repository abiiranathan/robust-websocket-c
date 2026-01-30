CC = gcc
CFLAGS = -Wall -Wextra -g -pthread -D_GNU_SOURCE -Iinclude
LDFLAGS = -lssl -lcrypto -pthread

# Directories
SRC_DIR = src
INC_DIR = include
ex_DIR = examples
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin
LIB_DIR = $(BUILD_DIR)/lib

# Target Library
LIB_NAME = libwebsocket.a
LIB_PATH = $(LIB_DIR)/$(LIB_NAME)

# Source and Objects
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Examples
CHAT_CLIENT_SRC = $(ex_DIR)/chat_client.c
CHAT_SERVER_SRC = $(ex_DIR)/chat_server.c

ECHO_SERVER_SRC = $(ex_DIR)/echo_server.c

CHAT_CLIENT_BIN = $(BIN_DIR)/chat_client
CHAT_SERVER_BIN = $(BIN_DIR)/chat_server
ECHO_SERVER_BIN = $(BIN_DIR)/echo_server

.PHONY: all clean directories

all: directories $(LIB_PATH) $(CHAT_CLIENT_BIN) $(CHAT_SERVER_BIN) $(ECHO_SERVER_BIN)

directories:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)

# Build Static Library
$(LIB_PATH): $(OBJS)
	ar rcs $@ $^

# Compile Library Objects
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build Examples
$(CHAT_CLIENT_BIN): $(CHAT_CLIENT_SRC) $(LIB_PATH)
	$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lwebsocket $(LDFLAGS)

$(CHAT_SERVER_BIN): $(CHAT_SERVER_SRC) $(LIB_PATH)
	$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lwebsocket $(LDFLAGS)

$(ECHO_SERVER_BIN): $(ECHO_SERVER_SRC) $(LIB_PATH)
	$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lwebsocket $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
