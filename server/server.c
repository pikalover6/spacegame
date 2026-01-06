// server/server.c - standalone authoritative game server (1 client, TCP)
// Protocol (line-based):
//   Client -> Server:
//     HELLO
//     INPUT <fwd> <right> <jump> <yawDelta> <pitchDelta> <dt>
//     CMD <text...>
//
//   Server -> Client:
//     WELCOME <version>
//     HIST <n>
//     LINE <text...>
//     STATE <x> <y> <z> <yaw> <pitch>

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define closesocket close
#endif

#include "../common/protocol.h"
#include "toy_term.h"

typedef struct {
    float x, y, z;
    float yaw, pitch;

    // Gravity/jump state (server authoritative)
    float vy;
    int grounded;
} PlayerState;

static int send_line(SOCKET s, const char* lineWithNewline) {
    int len = (int)strlen(lineWithNewline);
    int sent = 0;
    while (sent < len) {
        int r = send(s, lineWithNewline + sent, len - sent, 0);
        if (r <= 0) return 0;
        sent += r;
    }
    return 1;
}

static int recv_line(SOCKET s, char* out, int cap) {
    // simple byte-by-byte until \n
    int n = 0;
    while (n < cap - 1) {
        char ch;
        int r = recv(s, &ch, 1, 0);
        if (r <= 0) return 0;
        if (ch == '\n') break;
        if (ch != '\r') out[n++] = ch;
    }
    out[n] = '\0';
    return 1;
}

static void send_history(SOCKET c, ToyTerm* term) {
    char buf[512];
    int n = term_history_count(term);
    snprintf(buf, sizeof(buf), "HIST %d\n", n);
    send_line(c, buf);

    for (int i = 0; i < n; i++) {
        const char* ln = term_history_line(term, i);
        if (!ln) ln = "";
        snprintf(buf, sizeof(buf), "LINE %s\n", ln);
        send_line(c, buf);
    }
}

static void send_state(SOCKET c, const PlayerState* ps) {
    char buf[256];
    snprintf(buf, sizeof(buf), "STATE %.6f %.6f %.6f %.6f %.6f\n",
             ps->x, ps->y, ps->z, ps->yaw, ps->pitch);
    send_line(c, buf);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#endif

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        printf("socket() failed\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(27015);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int opt = 1;
#ifdef _WIN32
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (bind(listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("bind() failed\n");
        closesocket(listenSock);
        return 1;
    }

    if (listen(listenSock, 1) == SOCKET_ERROR) {
        printf("listen() failed\n");
        closesocket(listenSock);
        return 1;
    }

    printf("Server listening on port 27015...\n");

    struct sockaddr_in clientAddr;
#ifdef _WIN32
    int clientLen = (int)sizeof(clientAddr);
#else
    socklen_t clientLen = sizeof(clientAddr);
#endif

    SOCKET client = accept(listenSock, (struct sockaddr*)&clientAddr, &clientLen);
    if (client == INVALID_SOCKET) {
        printf("accept() failed\n");
        closesocket(listenSock);
        return 1;
    }

    printf("Client connected.\n");

    ToyTerm* term = term_create();

    PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.x = 0.0f;
    ps.y = 1.6f;   // "eye height"
    ps.z = 2.0f;
    ps.yaw = 0.0f;
    ps.pitch = 0.0f;
    ps.vy = 0.0f;
    ps.grounded = 1;

    // Welcome + initial sync
    send_line(client, "WELCOME " PROTO_VERSION "\n");
    send_history(client, term);
    send_state(client, &ps);

    char line[512];

    // Physics constants
    const float speed = 4.5f;
    const float gravity = 18.0f;
    const float jumpVel = 6.5f;
    const float groundY = 1.6f;   // standing eye height above "ground"

    while (recv_line(client, line, sizeof(line))) {
        if (strncmp(line, "HELLO", 5) == 0) {
            // no-op
        }
        else if (strncmp(line, "INPUT ", 6) == 0) {
            // INPUT fwd right jump yawDelta pitchDelta dt
            float fwd = 0.0f, right = 0.0f, up = 0.0f;
            float yawD = 0.0f, pitchD = 0.0f, dt = 0.0f;

            if (sscanf(line + 6, "%f %f %f %f %f %f", &fwd, &right, &up, &yawD, &pitchD, &dt) == 6) {
                // Look
                ps.yaw   += yawD;
                ps.pitch += pitchD;

                // Clamp pitch
                if (ps.pitch > 1.2f) ps.pitch = 1.2f;
                if (ps.pitch < -1.2f) ps.pitch = -1.2f;

                // Move in yaw plane
                float cy = cosf(ps.yaw), sy = sinf(ps.yaw);

                float fx = sy;
                float fz = cy;

                float rx = -cy;
                float rz = sy;

                ps.x += (fx * fwd + rx * right) * speed * dt;
                ps.y += up * speed * dt;
                ps.z += (fz * fwd + rz * right) * speed * dt;

                send_state(client, &ps);
            }
        }
        else if (strncmp(line, "CMD ", 4) == 0) {
            const char* cmd = line + 4;
            int before = term_history_count(term);
            term_run(term, cmd);
            int after = term_history_count(term);

            for (int i = before; i < after; i++) {
                const char* ln = term_history_line(term, i);
                char buf[512];
                snprintf(buf, sizeof(buf), "LINE %s\n", ln ? ln : "");
                send_line(client, buf);
            }
        }
        else {
            // ignore unknown
        }
    }

    printf("Client disconnected.\n");

    term_destroy(term);
    closesocket(client);
    closesocket(listenSock);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
