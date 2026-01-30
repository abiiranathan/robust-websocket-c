#include "../include/ws_client_lib.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

void ws_client_run(ws_client_t* client) {
    uint8_t buffer[4096];
    
    while (client->state != WS_STATE_CLOSED) {
        ssize_t n = read(client->socket_fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) continue;
            // Error
            if (client->on_error) client->on_error(client, "Read error");
            break;
        } else if (n == 0) {
            // EOF
            if (client->on_close) client->on_close(client, WS_STATUS_ABNORMAL, "Connection closed by peer");
            break;
        }
        
        ws_consume(client, buffer, (size_t)n);
    }
}
