#pragma once

// Platform-specific socket definitions and initialization

#ifdef _WIN32
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <mstcpip.h>
#   pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    typedef SOCKET socket_t;
#   define SOCKET_INVALID INVALID_SOCKET
#   define SOCKET_ERROR_CODE WSAGetLastError()
#   define close_socket closesocket
#   define poll WSAPoll
#else
#   include <sys/socket.h>
#   include <sys/time.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <netdb.h>
#   include <arpa/inet.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <poll.h>
#   include <errno.h>
    typedef int socket_t;
#   define SOCKET_INVALID -1
#   define SOCKET_ERROR_CODE errno
#   define close_socket close
#endif

namespace zenrx {

#ifdef _WIN32
// Windows socket initialization (thread-safe, idempotent)
inline bool initWinsock()
{
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return false;
        }
        initialized = true;
    }
    return true;
}
#else
// No-op on non-Windows platforms
inline bool initWinsock() { return true; }
#endif

} // namespace zenrx
