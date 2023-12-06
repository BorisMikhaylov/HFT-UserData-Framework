//
// Created by Boris Mikhaylov on 2023-11-17.
//

#include "fastsocket.h"

namespace bhft {

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

    status Socket::read(char *dst, size_t count) {
        while (begin != end && count-- > 0) {
            *dst++ = *begin++;
        }
        if (count == 0) return success;
        while (count > 0) {
            ssize_t cntReadBytes = recv(socket, readBuffer, sizeof readBuffer, 0);
            if (cntReadBytes < 0 && (socketerrno == SOCKET_EWOULDBLOCK || socketerrno == SOCKET_EAGAIN_EINPROGRESS)) {
                //TODO wait for data and PING if there's no data
                continue;
            } else if (cntReadBytes <= 0) {
                socketClosed = true;
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

    void bcopy(char *dst, const char *src, size_t count, uint8_t *mask, int index) {
        while (count--) {
            *dst++ = *src++ ^ mask[(index++) & 3];
        }
    }

    status Socket::read(char *dst, size_t count, uint8_t *mask) {
        size_t cnt = std::min((size_t) (end - begin), count);
        bcopy(dst, begin, cnt, mask, 0);
        count -= cnt;
        dst += cnt;
        begin += cnt;
        if (count == 0) return success;
        int index = cnt & 3;
        while (count > 0) {
            ssize_t cntReadBytes = recv(socket, readBuffer, sizeof readBuffer, 0);
            if (cntReadBytes < 0 && (socketerrno == SOCKET_EWOULDBLOCK || socketerrno == SOCKET_EAGAIN_EINPROGRESS)) {
                //TODO wait for data and PING if there's no data
                continue;
            } else if (cntReadBytes <= 0) {
                socketClosed = true;
                return closed;
            }
            begin = readBuffer;
            end = begin + cntReadBytes;
            cnt = std::min((size_t) (end - begin), count);
            bcopy(dst, begin, cnt, mask, index);
            index += cnt;
            count -= cnt;
            dst += cnt;
            begin += cnt;
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

    WebSocket::WebSocket(const std::string &hostname, int port, const std::string &path, bool useMask)
            : socket(hostname, port), useMask(useMask) {
        if (isClosed()) {
            return;
        }
        char buffer[4096];
        const char *format = "GET /%s HTTP/1.1\r\nHost: %s:%i\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
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

    status WebSocket::getMessage(Message &message) {
        wsheader_type ws;
        do {
            char header[16];
            if (socket.read(header, 2) == closed) return closed;
            const uint8_t *data = (uint8_t *) header;
            ws.fin = (data[0] & 0x80) == 0x80;
            ws.opcode = (wsheader_type::opcode_type) (data[0] & 0x0f);
            ws.mask = (data[1] & 0x80) == 0x80;
            ws.N0 = (data[1] & 0x7f);
            ws.header_size = 2 + (ws.N0 == 126 ? 2 : 0) + (ws.N0 == 127 ? 8 : 0) + (ws.mask ? 4 : 0);
            if (socket.read(header + 2, ws.header_size - 2) == closed) return closed;
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
                if (ws.mask) {
                    if (socket.read(message.end, ws.N, ws.masking_key) == closed) return closed;
                } else {
                    if (socket.read(message.end, ws.N) == closed) return closed;
                }
                message.end += ws.N;
            } else if (ws.opcode == wsheader_type::PING) {
                if (socket.read(message.end, ws.N) == closed) return closed;
                getOutputMessage().write(message.end, message.end + ws.N);
                sendLastOutputMessage(wsheader_type::PONG);
                continue;
            } else if (ws.opcode == wsheader_type::PONG) {
                if (socket.read(message.end, ws.N) == closed) return closed;
                continue;
            } else {
                socket.socketClosed = true;
                return closed;
            }
        } while (!ws.fin);
        *message.end = 0;
        return success;
    }

    status WebSocket::sendLastOutputMessage(wsheader_type::opcode_type type) {
        const uint8_t masking_key[4] = {0x12, 0x34, 0x56, 0x78};
        //cont uint8_t masking_key[4] = {0, 0, 0, 0};
        // TODO: consider acquiring a lock on txbuf...
        size_t messageSize = outputMessage.end - outputMessage.begin;

        int headerSize = 2 + (messageSize >= 126 ? 2 : 0) + (messageSize >= 65536 ? 6 : 0) + (useMask ? 4 : 0);
        auto *header = reinterpret_cast<uint8_t *>(outputMessage.begin - headerSize);
        header[0] = 0x80 | type;
        if (messageSize < 126) {
            header[1] = (messageSize & 0xff) | (useMask ? 0x80 : 0);
            if (useMask) {
                header[2] = masking_key[0];
                header[3] = masking_key[1];
                header[4] = masking_key[2];
                header[5] = masking_key[3];
            }
        } else if (messageSize < 65536) {
            header[1] = 126 | (useMask ? 0x80 : 0);
            header[2] = (messageSize >> 8) & 0xff;
            header[3] = (messageSize >> 0) & 0xff;
            if (useMask) {
                header[4] = masking_key[0];
                header[5] = masking_key[1];
                header[6] = masking_key[2];
                header[7] = masking_key[3];
            }
        } else { // TODO: run coverage testing here
            header[1] = 127 | (useMask ? 0x80 : 0);
            header[2] = (messageSize >> 56) & 0xff;
            header[3] = (messageSize >> 48) & 0xff;
            header[4] = (messageSize >> 40) & 0xff;
            header[5] = (messageSize >> 32) & 0xff;
            header[6] = (messageSize >> 24) & 0xff;
            header[7] = (messageSize >> 16) & 0xff;
            header[8] = (messageSize >> 8) & 0xff;
            header[9] = (messageSize >> 0) & 0xff;
            if (useMask) {
                header[10] = masking_key[0];
                header[11] = masking_key[1];
                header[12] = masking_key[2];
                header[13] = masking_key[3];
            }
        }
        // N.B. - txbuf will keep growing until it can be transmitted over the socket:
        if (useMask) {
// could be omitted when masking key is zeros
            auto m = *(unsigned int *) masking_key;
            for (size_t i = 0; i < messageSize; i += sizeof(unsigned)) {
                *(unsigned int *) (outputMessage.begin + i) ^= m;
            }

        }
        return socket.write(reinterpret_cast<const char *>(header), messageSize + headerSize);
    }

    OutputMessage &WebSocket::getOutputMessage() {
        outputMessage.reset();
        return outputMessage;
    }

    Message::Message(char *begin) : begin(begin), end(begin) {}
} // bhft