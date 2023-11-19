//
// Created by Boris Mikhaylov on 2023-11-17.
//

#include "fastsocket.h"

namespace bhft {
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

    Socket::Socket(const std::string &hostname, int port) : socket(INVALID_SOCKET), begin(readBuffer), end(readBuffer),
                                                            socketClosed(false) {
        struct addrinfo hints;
        struct addrinfo *result;
        struct addrinfo *p;
        int ret;
        char sport[16];
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(sport, 16, "%d", port);
        if ((ret = getaddrinfo(hostname.c_str(), sport, &hints, &result)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
            socketClosed = true;
            return;
        }
        for (p = result; p != nullptr; p = p->ai_next) {
            socket = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket == INVALID_SOCKET) { continue; }
            if (connect(socket, p->ai_addr, p->ai_addrlen) != SOCKET_ERROR) {
                break;
            }
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        freeaddrinfo(result);
    }

    Socket::~Socket() {
        closesocket(socket);
    }

    status Socket::read(char *dst, int count) {
        while (begin != end && count-- > 0) {
            *dst++ = *begin++;
        }
        if (count == 0) return success;
        while (count > 0) {
            ssize_t cntReadBytes = recv(socket, readBuffer, sizeof readBuffer, 0);
            if (cntReadBytes < 0 && (socketerrno == SOCKET_EWOULDBLOCK || socketerrno == SOCKET_EAGAIN_EINPROGRESS)) {
                continue;
            } else if (cntReadBytes <= 0) {
                return closed;
            }
            begin = readBuffer;
            end = begin + cntReadBytes;
            while (begin != end && count-- > 0) {
                *dst++ = *begin++;
            }
        }
        return success;
    }

    status Socket::write(const char *src, int count) {
        while (count > 0) {
            int ret = ::send(socket, src, count, 0);
            if (ret < 0 && (socketerrno == SOCKET_EWOULDBLOCK || socketerrno == SOCKET_EAGAIN_EINPROGRESS)) {
                continue;
            } else if (ret <= 0) {
                return closed;
            }
            count -= ret;
            src += ret;
        }
        return success;
    }

    WebSocket::WebSocket(const std::string &hostname, int port, const std::string &path, bool mask)
            : socket(hostname, port) {
        if (isClosed()) {
            return;
        }
        char buffer[4096];
        char *format = "GET /%s HTTP/1.1\r\nHost: %s:%i\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        sprintf(buffer, format, path.c_str(), hostname.c_str(), port);
        socket.write(buffer, strlen(buffer));
        while (strlen(buffer) > 0) {
            if (readLine(buffer) == closed) {
                return;
            }
            std::cout << buffer << std::endl;
        }
        int flag = 1;
        ::setsockopt(socket.socket, IPPROTO_TCP, TCP_NODELAY, (char *) &flag,
                     sizeof(flag)); // Disable Nagle's algorithm
        ::fcntl(socket.socket, F_SETFL, O_NONBLOCK);
    }

    status WebSocket::readLine(char *buffer) {
        if (socket.read(buffer, 2) == closed) {
            return closed;
        }
        char *ptr = buffer;
        while (ptr[0] != '\r' || ptr[1] != '\n') {
            if (socket.read(ptr++ + 2, 1) == closed) {
                return closed;
            }
        }
        *ptr = 0;
        return success;
    }

    status WebSocket::getMessage(char *dst) {
        wsheader_type ws;
        do {
            char header[16];
            socket.read(header, 2);
            const uint8_t *data = (uint8_t *) header;
            ws.fin = (data[0] & 0x80) == 0x80;
            ws.opcode = (wsheader_type::opcode_type) (data[0] & 0x0f);
            ws.mask = (data[1] & 0x80) == 0x80;
            ws.N0 = (data[1] & 0x7f);
            ws.header_size = 2 + (ws.N0 == 126 ? 2 : 0) + (ws.N0 == 127 ? 8 : 0) + (ws.mask ? 4 : 0);
            socket.read(header + 2, ws.header_size - 2);
            int i = 0;
            if (ws.N0 < 126) {
                ws.N = ws.N0;
                i = 2;
            } else if (ws.N0 == 126) {
                ws.N = 0;
                ws.N |= ((uint64_t) data[2]) << 8;
                ws.N |= ((uint64_t) data[3]) << 0;
                i = 4;
            } else if (ws.N0 == 127) {
                ws.N = 0;
                ws.N |= ((uint64_t) data[2]) << 56;
                ws.N |= ((uint64_t) data[3]) << 48;
                ws.N |= ((uint64_t) data[4]) << 40;
                ws.N |= ((uint64_t) data[5]) << 32;
                ws.N |= ((uint64_t) data[6]) << 24;
                ws.N |= ((uint64_t) data[7]) << 16;
                ws.N |= ((uint64_t) data[8]) << 8;
                ws.N |= ((uint64_t) data[9]) << 0;
                i = 10;
                if (ws.N & 0x8000000000000000ull) {
                    // https://tools.ietf.org/html/rfc6455 writes the "the most
                    // significant bit MUST be 0."
                    //
                    // We can't drop the frame, because (1) we don't we don't
                    // know how much data to skip over to find the next header,
                    // and (2) this would be an impractically long length, even
                    // if it were valid. So just close() and return immediately
                    // for now.
                    socket.socketClosed = true;
                    return closed;
                }
            }
            if (ws.mask) {
                ws.masking_key[0] = ((uint8_t) data[i + 0]) << 0;
                ws.masking_key[1] = ((uint8_t) data[i + 1]) << 0;
                ws.masking_key[2] = ((uint8_t) data[i + 2]) << 0;
                ws.masking_key[3] = ((uint8_t) data[i + 3]) << 0;
            } else {
                ws.masking_key[0] = 0;
                ws.masking_key[1] = 0;
                ws.masking_key[2] = 0;
                ws.masking_key[3] = 0;
            }
            // We got a whole message, now do something with it:
            if (
                    ws.opcode == wsheader_type::TEXT_FRAME
                    || ws.opcode == wsheader_type::BINARY_FRAME
                    || ws.opcode == wsheader_type::CONTINUATION
                    ) {
                socket.read(dst, ws.N);
                if (ws.mask) {
                    for (size_t i = 0; i != ws.N; ++i) {
                        dst[i] ^= ws.masking_key[i & 0x3];
                    }
                }
                dst += ws.N;
            } else if (ws.opcode == wsheader_type::PING) {
//            if (ws.mask) {
//                for (size_t i = 0; i != ws.N; ++i) {
//                    rxbuf[i + ws.header_size] ^= ws.masking_key[i & 0x3];
//                }
//            }
//            std::string data(rxbuf.begin() + ws.header_size, rxbuf.begin() + ws.header_size + (size_t) ws.N);
//            sendData(wsheader_type::PONG, data.size(), data.begin(), data.end());
                socket.socketClosed = true;
                return closed;
                // TODO implement
            } else if (ws.opcode == wsheader_type::PONG) {
                socket.read(dst, ws.N);
                continue;
            } else {
                socket.socketClosed = true;
                return closed;
            }
        } while (!ws.fin);
    }

} // bhft

int main() {
    bhft::WebSocket ws("127.0.0.1", 9999, "?url=wss://ws.okx.com:8443/ws/v5/private", true);
}