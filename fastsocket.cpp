//
// Created by Boris Mikhaylov on 2023-11-17.
//

#include "fastsocket.h"

namespace bhft {
    Socket::Socket(const std::string &hostname, int port) : socket(INVALID_SOCKET), begin(readBuffer), end(readBuffer), socketClosed(false) {
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
        if(isClosed()){
            return;
        }
        char buffer[4096];
        char *format = "GET /%s HTTP/1.1\r\nHost: %s:%i\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        sprintf(buffer, format, path.c_str(), hostname.c_str(), port);
        socket.write(buffer, strlen(buffer));
        while (strlen(buffer) > 0){
            if (readLine(buffer) == closed){
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
        if (socket.read(buffer, 2) == closed){
            return closed;
        }
        char* ptr = buffer;
        while (ptr[0] != '\r' || ptr[1] != '\n'){
            if (socket.read(ptr++ + 2, 1) == closed){
                return closed;
            }
        }
        *ptr = 0;
        return success;
    }
} // bhft

int main(){
    bhft::WebSocket ws("127.0.0.1", 9999, "?url=wss://ws.okx.com:8443/ws/v5/private", true);
}