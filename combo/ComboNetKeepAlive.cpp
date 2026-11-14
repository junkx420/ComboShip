// combo/ComboNetKeepAlive.cpp
//
// Co-op hardening (2026-11-13): TCP keepalive for the launcher-owned Anchor socket.
//
// SDL_net exposes no socket options, and a TCP connection whose peer silently vanished (Wi-Fi drop,
// router reset, laptop sleep) stays "connected" until the OS gives up on it — with no outbound data
// that is never. Keepalive probes make the OS fail the socket within ~30 s, which ends ComboAnchor's
// receive loop and triggers the normal reconnect + resync.
//
// Kept in its own translation unit so the Winsock headers never meet ComboShip.cpp's <Windows.h>
// include order.
//
// SDL_net's TCPsocket is an opaque pointer to
//     struct _TCPsocket { int ready; SOCKET channel; IPaddress remoteAddress; IPaddress localAddress; int sflag; };
// (SDLnetTCP.c, unchanged across SDL_net 2.0.x - 2.2.x). The handle is read through that layout and
// then VALIDATED with getsockopt(SO_TYPE) == SOCK_STREAM before anything is set, so a layout change in
// a future SDL_net degrades to "no keepalive" (function returns false), never to touching a random
// handle. Failure is non-fatal: the caller only logs it.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
typedef SOCKET ComboRawSocket;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
typedef int ComboRawSocket;
#endif

namespace {
struct SdlNetTcpSocketLayout {
    int ready;
    ComboRawSocket channel;
};

[[maybe_unused]] constexpr int kIdleSeconds = 15;    // first probe after this much silence
[[maybe_unused]] constexpr int kIntervalSeconds = 3; // between unanswered probes
[[maybe_unused]] constexpr int kProbeCount = 5;      // probes before the OS drops the link (POSIX; Windows uses 10)
[[maybe_unused]] constexpr int kMaxRetransmitSeconds = 30; // cap on unacknowledged-data retries (Windows TCP_MAXRT)
} // namespace

bool ComboNet_EnableKeepAlive(void* sdlTcpSocket) {
    if (sdlTcpSocket == nullptr)
        return false;
    const ComboRawSocket s = static_cast<SdlNetTcpSocketLayout*>(sdlTcpSocket)->channel;

    // Validate before touching anything: must be a live stream socket.
    int type = 0;
#ifdef _WIN32
    int typeLen = sizeof(type);
    if (s == INVALID_SOCKET || getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen) != 0)
        return false;
#else
    socklen_t typeLen = sizeof(type);
    if (s < 0 || getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &typeLen) != 0)
        return false;
#endif
    if (type != SOCK_STREAM)
        return false;

    int on = 1;
#ifdef _WIN32
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on)) != 0)
        return false;
    tcp_keepalive ka = {};
    ka.onoff = 1;
    ka.keepalivetime = kIdleSeconds * 1000;
    ka.keepaliveinterval = kIntervalSeconds * 1000;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytes, nullptr, nullptr);
#ifdef TCP_MAXRT
    // Also bound how long unacknowledged game traffic is retried before the send fails.
    int maxRt = kMaxRetransmitSeconds;
    setsockopt(s, IPPROTO_TCP, TCP_MAXRT, reinterpret_cast<const char*>(&maxRt), sizeof(maxRt));
#endif
#else
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0)
        return false;
#if defined(TCP_KEEPIDLE)
    int idle = kIdleSeconds;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE) // macOS spelling of the idle time
    int idle = kIdleSeconds;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = kIntervalSeconds;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = kProbeCount;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
#ifdef TCP_USER_TIMEOUT
    unsigned int userTimeoutMs = kMaxRetransmitSeconds * 1000u;
    setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeoutMs, sizeof(userTimeoutMs));
#endif
#endif
    return true;
}
