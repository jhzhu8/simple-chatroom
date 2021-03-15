#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <netinet/in.h> 
#include <sys/socket.h> 
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

#include "chatroom.h"

#define TCP_PORT_MIN        (49512)
#define TCP_PORT_MAX        (65535)
#define DEFAULT_TCP_PORT


int main(int argc, char* argv[])
{
    if(argc > 2) {
        printf("Usage: chat_server [opt: port]\n");
        return -1;
    }
    
    uint32_t port = 0;
    if(argc == 2) {
        port = strtoul(argv[1], NULL, 10);
        if(port > TCP_PORT_MAX || port < TCP_PORT_MIN) {
            printf("ERROR: Invalid port. Please pick port between %u and %u\n", TCP_PORT_MIN, TCP_PORT_MAX);
            return -1;
        }
    } else {
        port = 1234;
        printf("INFO: Using default port %u\n", port);
    }

    int listen_fd = -1;
    int connect_fd = -1;
    struct sockaddr_in serverAddr, clientAddr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0) {
        int err = errno;
        printf("ERROR: Failed to create listening socket with err=%d\n", err);
        return -1;
    }

    // Bind
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(port);
    if(bind(listen_fd, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) != 0) {
        int err = errno;
        printf("ERROR: Failed to bind listening socket on port %u with err=%d\n", port, err);
        close(listen_fd);
        return -1;
    }

    // Listening
    if(listen(listen_fd, 20) != 0) {
        int err = errno;
        printf("ERROR: Failed to listen on port %u with err=%d\n", port, err);
        close(listen_fd);
        return -1;
    }

    while(1) {
        size_t addrLen = sizeof(clientAddr);
        connect_fd = accept(listen_fd, (struct sockaddr *)&clientAddr, (socklen_t*)&addrLen);
        if(connect_fd < 0) {
            printf("Failed to accept connection. Retrying.\n");
        }

        if(new_connection(connect_fd) < 0) {
            printf("Failed to add fd %d to chatroom\n", connect_fd);
            close(connect_fd);
        }
    }
}