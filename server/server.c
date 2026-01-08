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

#define MAX_OBJS 256

typedef struct {
    int id;
    float x,y,z;
    float s;
    int r,g,b;
    int alive;
} ObjCube;

static ObjCube g_objs[MAX_OBJS];
static int g_nextObjId = 1;

static ObjCube* obj_alloc(void) {
    for (int i = 0; i < MAX_OBJS; i++) {
        if (!g_objs[i].alive) {
            g_objs[i].alive = 1;
            g_objs[i].id = g_nextObjId++;
            return &g_objs[i];
        }
    }
    return NULL;
}

static int send_line(SOCKET s, const char* lineWithNewline);

static void send_obj_add(SOCKET c, const ObjCube* o) {
    char buf[256];
    snprintf(buf, sizeof(buf), "OBJ_ADD %d %.3f %.3f %.3f %.3f %d %d %d\n",
             o->id, o->x, o->y, o->z, o->s, o->r, o->g, o->b);
    send_line(c, buf);
}

static void send_all_objs(SOCKET c) {
    // could send OBJ_CLEAR first if you want strict sync
    for (int i = 0; i < MAX_OBJS; i++) {
        if (g_objs[i].alive) send_obj_add(c, &g_objs[i]);
    }
}

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

static int http_post_localhost_8080(const char* path, const char* jsonBody, char* out, int outCap) {
    // Connect to 127.0.0.1:8080 and POST jsonBody to path. Store full HTTP response in out.
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }

    char req[8192];
    int bodyLen = (int)strlen(jsonBody);
    int n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, bodyLen, jsonBody);

    if (n <= 0 || n >= (int)sizeof(req)) {
        closesocket(s);
        return 0;
    }

    int sent = 0;
    while (sent < n) {
        int r = send(s, req + sent, n - sent, 0);
        if (r <= 0) { closesocket(s); return 0; }
        sent += r;
    }

    int total = 0;
    while (total < outCap - 1) {
        int r = recv(s, out + total, outCap - 1 - total, 0);
        if (r <= 0) break;
        total += r;
    }
    out[total] = 0;

    closesocket(s);
    return total > 0;
}

static int json_extract_content_field(const char* httpResp, char* out, int outCap) {
    // extremely naive: find "content": then extract the JSON string value
    const char* p = strstr(httpResp, "\"content\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')) p++;
    if (*p != '\"') return 0;
    p++; // skip opening quote

    int w = 0;
    while (*p && w < outCap - 1) {
        if (*p == '\"') { out[w] = 0; return 1; }
        if (*p == '\\') {
            p++;
            if (!*p) break;
            // handle a few escapes
            if (*p == 'n') out[w++] = '\n';
            else if (*p == 'r') out[w++] = '\r';
            else if (*p == 't') out[w++] = '\t';
            else out[w++] = *p;
            p++;
            continue;
        }
        out[w++] = *p++;
    }
    out[w] = 0;
    return 1;
}

static int llm_make_command(const char* userText, char* outCmd, int outCap) {
    // Ask llama-server /completion to output ONE line like:
    // SPAWN_CUBE x y z size r g b
    // Keep it short and parseable.

    char prompt[2048];
    snprintf(prompt, sizeof(prompt),
        "You are a command generator for a tiny 3D room toy.\n"
        "Output exactly ONE line. No extra text.\n"
        "Allowed commands:\n"
        "  SPAWN_CUBE x y z size r g b\n"
        "Notes:\n"
        "- Coordinates are floats.\n"
        "- size is float.\n"
        "- r g b are integers 0..255.\n"
        "- If the request is unclear, choose a reasonable default near (0,1,6).\n"
        "User request: %s\n"
        "Command:",
        userText);

    // JSON request for llama-server /completion
    // Escape quotes minimally by banning them in userText (fine for a toy), or you can escape properly later.
    // We'll just be cautious and replace double quotes.
    char safePrompt[2048];
    int w=0;
    for (int i=0; prompt[i] && w < (int)sizeof(safePrompt)-1; i++) {
        char c = prompt[i];
        if (c == '\"') c = '\'';
        safePrompt[w++] = c;
    }
    safePrompt[w] = 0;

    char body[4096];
    snprintf(body, sizeof(body),
        "{"
        "\"prompt\":\"%s\","
        "\"n_predict\":64,"
        "\"temperature\":0.2,"
        "\"stop\":[\"\\n\"]"
        "}",
        safePrompt);

    char resp[16384];
    if (!http_post_localhost_8080("/completion", body, resp, (int)sizeof(resp))) {
        return 0;
    }

    char content[1024];
    if (!json_extract_content_field(resp, content, (int)sizeof(content))) {
        return 0;
    }

    // Trim leading/trailing whitespace
    char* s = content;
    while (*s && (*s==' ' || *s=='\t' || *s=='\r' || *s=='\n')) s++;
    int len = (int)strlen(s);
    while (len>0 && (s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\r' || s[len-1]=='\n')) s[--len] = 0;

    strncpy(outCmd, s, outCap-1);
    outCmd[outCap-1] = 0;
    return 1;
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

    send_all_objs(client);

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

            // 1) manual spawn: "spawn x y z"
            if (strncmp(cmd, "spawn ", 6) == 0) {
                float x=0,y=1,z=6;
                if (sscanf(cmd + 6, "%f %f %f", &x, &y, &z) < 1) {
                    send_line(client, "LINE Error: usage spawn x y z\n");
                    send_line(client, "LINE >>> \n");
                    continue;
                }

                ObjCube* o = obj_alloc();
                if (!o) {
                    send_line(client, "LINE Error: object limit reached\n");
                    send_line(client, "LINE >>> \n");
                    continue;
                }

                o->x=x; o->y=y; o->z=z;
                o->s=1.0f;
                o->r=200; o->g=200; o->b=255;

                send_obj_add(client, o);
                send_line(client, "LINE Spawned cube.\n");
                send_line(client, "LINE >>> \n");
                continue;
            }

            // 2) ai command: "ai <text...>"
            if (strncmp(cmd, "ai ", 3) == 0) {
                const char* userText = cmd + 3;

                send_line(client, "LINE (thinking...)\n");

                char outCmd[512];
                if (!llm_make_command(userText, outCmd, (int)sizeof(outCmd))) {
                    send_line(client, "LINE Error: LLM request failed. Is llama-server running on 127.0.0.1:8080?\n");
                    send_line(client, "LINE >>> \n");
                    continue;
                }

                char echo[768];
                snprintf(echo, sizeof(echo), "LINE LLM: %s\n", outCmd);
                send_line(client, echo);

                // Parse: SPAWN_CUBE x y z size r g b
                if (strncmp(outCmd, "SPAWN_CUBE", 10) == 0) {
                    float x=0,y=1,z=6,s=1;
                    int r=200,g=200,b=200;
                    if (sscanf(outCmd + 10, "%f %f %f %f %d %d %d", &x, &y, &z, &s, &r, &g, &b) >= 4) {
                        if (s < 0.1f) s = 0.1f;
                        if (s > 5.0f) s = 5.0f;
                        if (r<0) r=0; if (r>255) r=255;
                        if (g<0) g=0; if (g>255) g=255;
                        if (b<0) b=0; if (b>255) b=255;

                        ObjCube* o = obj_alloc();
                        if (!o) {
                            send_line(client, "LINE Error: object limit reached\n");
                        } else {
                            o->x=x; o->y=y; o->z=z;
                            o->s=s;
                            o->r=r; o->g=g; o->b=b;
                            send_obj_add(client, o);
                            send_line(client, "LINE Done.\n");
                        }
                    } else {
                        send_line(client, "LINE Error: could not parse SPAWN_CUBE\n");
                    }
                } else {
                    send_line(client, "LINE Error: unsupported LLM command\n");
                }

                send_line(client, "LINE >>> \n");
                continue;
            }

            // Fallback: keep existing toy interpreter
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
