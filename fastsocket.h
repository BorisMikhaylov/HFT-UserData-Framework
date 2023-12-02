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
#include <cstring>

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR   (-1)
#endif
#define closesocket(s) ::close(s)

#include <errno.h>

#define BTRACE std::cout << "Tracing " << __FILE__ << ":" << __LINE__ << std::endl;

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

    struct wsheader_type {
        unsigned header_size;
        bool fin;
        bool mask;
        enum opcode_type {
            CONTINUATION = 0x0,
            TEXT_FRAME = 0x1,
            BINARY_FRAME = 0x2,
            CLOSE = 8,
            PING = 9,
            PONG = 0xa,
        } opcode;
        int N0;
        uint64_t N;
        uint8_t masking_key[4];
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

        bool isClosed() {
            return socketClosed;
        }
    };

    struct OutputMessage {
        char buffer[4000];
        char *begin;
        char *end;

        void reset() {
            begin = buffer + 16;
            end = begin;
        }

        void write(char ch) {
            *end++ = ch;
        }

        void write(const char *str) {
            while (*str != 0) {
                *end++ = *str++;
            }
        }

        void write(const char *start, const char *finish){
            while (start != finish){
                *end++ = *start++;
            }
        }
    };

    struct WebSocket {
        Socket socket;
        bool useMask;
        OutputMessage outputMessage;

        explicit WebSocket(const std::string &hostname, int port, const std::string &path, bool useMask);

        status readLine(char *buffer);

        bool isClosed() {
            return socket.isClosed();
        }

        status getMessage(char *dst);

        OutputMessage &getOutputMessage();

        status sendLastOutputMessage(wsheader_type::opcode_type type);
    };

} // bhft

#endif //HFT_FRAMEWORK_USERDATA_FASTSOCKET_H
