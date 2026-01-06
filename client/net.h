#ifndef NET_H
#define NET_H

#include <stdint.h>   // <-- for uint16_t

#ifdef _WIN32
  // Prevent windows.h from pulling in GDI/USER stuff that conflicts with raylib
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef NOGDI
  #define NOGDI
  #endif
  #ifndef NOUSER
  #define NOUSER
  #endif

  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET net_socket_t;
#else
  typedef int net_socket_t;
#endif

typedef struct {
    net_socket_t s;
    int connected;
} NetClient;

int  net_init(void);
void net_shutdown(void);

int  net_connect(NetClient* c, const char* host, uint16_t port);
void net_close(NetClient* c);

int  net_sendf(NetClient* c, const char* fmt, ...);

// Poll available lines (non-blocking)
int  net_poll_lines(NetClient* c, void (*on_line)(const char*, void*), void* userdata);

#endif // NET_H
