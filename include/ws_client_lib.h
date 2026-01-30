#ifndef WS_CLIENT_LIB_H
#define WS_CLIENT_LIB_H

#include "websocket.h"

/**
 * A simple helper to run a client in a blocking loop (or thread).
 * This manages the read loop for you.
 * 
 * Usage:
 *   ws_client_t client;
 *   ws_init(&client);
 *   // set callbacks...
 *   ws_connect(&client, ...);
 *   ws_client_run(&client); // Blocks until closed
 */
void ws_client_run(ws_client_t* client);

#endif // WS_CLIENT_LIB_H
