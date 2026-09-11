/*
 * Standalone dummy build of pc_tcp_server.c.
 *
 * Build:
 *   gcc -Wall -Wextra -std=c11 pc_tcp_server_dummy.c -o pc_tcp_server_dummy.exe -lws2_32 -lwlanapi
 *
 * This wrapper starts the shared server implementation with its --dummy mode,
 * so the dashboard, CSV session handling, and command timeline are exercised
 * without a Pico W or a TCP connection on port 4242.
 */
#define main pc_tcp_server_real_main
#include "pc_tcp_server.c"
#undef main

int main(void) {
    char program_name[] = "pc_tcp_server_dummy.exe";
    char dummy_argument[] = "--dummy";
    char *arguments[] = { program_name, dummy_argument };
    return pc_tcp_server_real_main(2, arguments);
}
