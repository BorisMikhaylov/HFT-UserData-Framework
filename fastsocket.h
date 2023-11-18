//
// Created by Boris Mikhaylov on 2023-11-17.
//

#ifndef HFT_FRAMEWORK_USERDATA_FASTSOCKET_H
#define HFT_FRAMEWORK_USERDATA_FASTSOCKET_H

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <iostream>

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR   (-1)
#endif
#define closesocket(s) ::close(s)

#include <errno.h>

#define socketerrno errno
#define SOCKET_EAGAIN_EINPROGRESS EAGAIN
#define SOCKET_EWOULDBLOCK EWOULDBLOCK


namespace bhft {
#ifndef _SOCKET_T_DEFINED
    typedef int socket_t;
#define _SOCKET_T_DEFINED
#endif


    enum status {
        success,
        closed
    };

    struct Socket {
        char readBuffer[1024];
        char *begin;
        char *end;
        socket_t socket;
        bool socketClosed;

        status read(char *dst, int count);

        status write(const char *src, int count);

        ~Socket();

        Socket(const std::string &hostname, int port);

        bool isClosed(){
            return socketClosed;
        }
    };

    struct WebSocket{
        Socket socket;
        bool mask;

        explicit WebSocket(const std::string &hostname, int port, const std::string &path, bool mask);

        status readLine(char* buffer);

        bool isClosed(){
            return socket.isClosed();
        }
    };

} // bhft

#endif //HFT_FRAMEWORK_USERDATA_FASTSOCKET_H
