#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#define _CRT_SECURE_NO_WARNINGS
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define closesocket close
#endif

static void set_nonblocking(net_socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

int net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2,2), &wsa) == 0;
#else
    return 1;
#endif
}

void net_shutdown(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

int net_connect(NetClient* c, const char* host, uint16_t port) {
    memset(c, 0, sizeof(*c));
    c->s = socket(AF_INET, SOCK_STREAM, 0);
    if (c->s == INVALID_SOCKET) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(host);
#else
    inet_pton(AF_INET, host, &addr.sin_addr);
#endif

    if (connect(c->s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
#ifdef _WIN32
        int e = WSAGetLastError();
        // connect should be blocking here; if it fails it's a real fail
        (void)e;
#endif
        closesocket(c->s);
        c->s = INVALID_SOCKET;
        return 0;
    }

    set_nonblocking(c->s);
    c->connected = 1;
    return 1;
}

void net_close(NetClient* c) {
    if (c->connected) {
        closesocket(c->s);
        c->connected = 0;
    }
}

int net_sendf(NetClient* c, const char* fmt, ...) {
    if (!c->connected) return 0;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int len = (int)strlen(buf);
    int sent = 0;
    while (sent < len) {
        int r = send(c->s, buf + sent, len - sent, 0);
        if (r <= 0) return 0;
        sent += r;
    }
    return 1;
}

static char g_accum[4096];
static int  g_accumLen = 0;

int net_poll_lines(NetClient* c, void (*on_line)(const char*, void*), void* userdata) {
    if (!c->connected) return 0;

    char tmp[1024];
    for (;;) {
        int r = recv(c->s, tmp, (int)sizeof(tmp), 0);
        if (r > 0) {
            if (g_accumLen + r >= (int)sizeof(g_accum)) {
                // overflow; reset
                g_accumLen = 0;
            }
            memcpy(g_accum + g_accumLen, tmp, r);
            g_accumLen += r;

            // extract lines
            int start = 0;
            for (int i=0;i<g_accumLen;i++) {
                if (g_accum[i] == '\n') {
                    int len = i - start;
                    if (len > 0 && g_accum[start + len - 1] == '\r') len--;
                    char line[1024];
                    if (len >= (int)sizeof(line)) len = (int)sizeof(line)-1;
                    memcpy(line, g_accum + start, len);
                    line[len] = '\0';
                    on_line(line, userdata);
                    start = i + 1;
                }
            }
            // shift remaining
            if (start > 0) {
                memmove(g_accum, g_accum + start, g_accumLen - start);
                g_accumLen -= start;
            }
            continue;
        }

#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) return 1;
#else
        if (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return 1;
#endif
        // disconnected
        c->connected = 0;
        return 0;
    }
}
