#include "toy_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <unistd.h>
  #include <errno.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define closesocket close
#endif

// -------------------- tweakables --------------------

// Your llama-server address:
#define LLM_HOST "127.0.0.1"
#define LLM_PORT 8080
#define LLM_PATH "/v1/chat/completions"

// Change these if your client expects different tokens:
#define LINE_OBJ_ADD   "OBJ_ADD"
#define LINE_OBJ_DEL   "OBJ_DEL"
#define LINE_OBJ_CLEAR "OBJ_CLEAR"

// How much chat memory to keep (toy):
#define MAX_CHAT_MSGS   48
#define MAX_ROLE_LEN    8
#define MAX_TEXT_LEN    1024

// ----------------------------------------------------

typedef struct {
    char role[MAX_ROLE_LEN];   // "system" | "user" | "assistant"
    char text[MAX_TEXT_LEN];
} ChatMsg;

struct ToyTerm {
    char history[TERM_HISTORY_MAX][TERM_LINE_MAX];
    int  histCount;

    ChatMsg chat[MAX_CHAT_MSGS];
    int chatCount;

    // auto-increment cube id if model omits id (optional)
    int nextCubeId;
};

// -------------------- history --------------------

static void hist_push(ToyTerm *t, const char *line) {
    if (!t || !line) return;

    if (t->histCount < TERM_HISTORY_MAX) {
        strncpy(t->history[t->histCount], line, TERM_LINE_MAX - 1);
        t->history[t->histCount][TERM_LINE_MAX - 1] = '\0';
        t->histCount++;
        return;
    }

    // shift up
    for (int i = 1; i < TERM_HISTORY_MAX; i++) {
        strcpy(t->history[i - 1], t->history[i]);
    }
    strncpy(t->history[TERM_HISTORY_MAX - 1], line, TERM_LINE_MAX - 1);
    t->history[TERM_HISTORY_MAX - 1][TERM_LINE_MAX - 1] = '\0';
}

static void replace_last(ToyTerm *t, const char *line) {
    if (!t || t->histCount <= 0) return;
    strncpy(t->history[t->histCount - 1], line, TERM_LINE_MAX - 1);
    t->history[t->histCount - 1][TERM_LINE_MAX - 1] = '\0';
}

// -------------------- tiny helpers --------------------

static void trim_ws(const char **p) {
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

static void chat_push(ToyTerm *t, const char *role, const char *text) {
    if (!t || !role || !text) return;

    if (t->chatCount < MAX_CHAT_MSGS) {
        strncpy(t->chat[t->chatCount].role, role, MAX_ROLE_LEN - 1);
        t->chat[t->chatCount].role[MAX_ROLE_LEN - 1] = '\0';
        strncpy(t->chat[t->chatCount].text, text, MAX_TEXT_LEN - 1);
        t->chat[t->chatCount].text[MAX_TEXT_LEN - 1] = '\0';
        t->chatCount++;
        return;
    }

    // drop oldest user/assistant pair-ish: shift left by 1
    for (int i = 1; i < MAX_CHAT_MSGS; i++) {
        t->chat[i - 1] = t->chat[i];
    }
    strncpy(t->chat[MAX_CHAT_MSGS - 1].role, role, MAX_ROLE_LEN - 1);
    t->chat[MAX_CHAT_MSGS - 1].role[MAX_ROLE_LEN - 1] = '\0';
    strncpy(t->chat[MAX_CHAT_MSGS - 1].text, text, MAX_TEXT_LEN - 1);
    t->chat[MAX_CHAT_MSGS - 1].text[MAX_TEXT_LEN - 1] = '\0';
}

static int is_printable_ascii(int c) {
    return (c >= 32 && c <= 126);
}

// JSON string escape into dst (no allocations)
static void json_escape(const char *src, char *dst, int cap) {
    int w = 0;
    for (int i = 0; src[i] && w < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '"') {
            if (w < cap - 2) { dst[w++] = '\\'; dst[w++] = (char)c; }
        } else if (c == '\n') {
            if (w < cap - 2) { dst[w++] = '\\'; dst[w++] = 'n'; }
        } else if (c == '\r') {
            if (w < cap - 2) { dst[w++] = '\\'; dst[w++] = 'r'; }
        } else if (c == '\t') {
            if (w < cap - 2) { dst[w++] = '\\'; dst[w++] = 't'; }
        } else if (is_printable_ascii(c)) {
            dst[w++] = (char)c;
        } else {
            // drop other bytes (toy)
        }
    }
    dst[w] = '\0';
}

// Very small JSON string extractor: expects p points at first quote of a JSON string.
// Returns pointer after closing quote, and writes unescaped content to out.
static const char *json_read_string(const char *p, char *out, int outCap) {
    if (!p || *p != '"') return NULL;
    p++; // skip opening quote

    int w = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            if (!e) break;
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = e; break;
            }
        }
        if (w < outCap - 1) out[w++] = c;
    }
    out[w] = '\0';
    if (*p != '"') return NULL;
    return p + 1; // after closing quote
}

static const char *find_json_key(const char *json, const char *key) {
    // super naive: finds first occurrence of "key"
    // good enough for toy controlled outputs
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return strstr(json, pat);
}

// -------------------- HTTP client (minimal) --------------------

static int sock_connect(const char *host, int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(host);
#else
    inet_pton(AF_INET, host, &addr.sin_addr);
#endif

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return (int)s;
}

static int sock_send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r <= 0) return 0;
        sent += r;
    }
    return 1;
}

static int sock_recv_some(SOCKET s, char *buf, int cap) {
    int r = recv(s, buf, cap, 0);
    return r;
}

static int parse_content_length(const char *hdrs) {
    const char *p = strstr(hdrs, "Content-Length:");
    if (!p) p = strstr(hdrs, "content-length:");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return atoi(p);
}

static int header_is_chunked(const char *hdrs) {
    return (strstr(hdrs, "Transfer-Encoding: chunked") ||
            strstr(hdrs, "transfer-encoding: chunked")) ? 1 : 0;
}

// Read HTTP response body into out (cap). Returns 1 ok.
static int http_post_json(const char *host, int port, const char *path,
                          const char *jsonBody, char *out, int outCap,
                          char *err, int errCap) {
    if (err && errCap) err[0] = '\0';
    if (out && outCap) out[0] = '\0';

    SOCKET s = (SOCKET)sock_connect(host, port);
    if (s == INVALID_SOCKET) {
        snprintf(err, errCap, "Could not connect to llama-server at %s:%d", host, port);
        return 0;
    }

    char req[8192];
    int bodyLen = (int)strlen(jsonBody);

    int n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        path, host, port, bodyLen, jsonBody);

    if (n <= 0 || n >= (int)sizeof(req)) {
        closesocket(s);
        snprintf(err, errCap, "Request too large");
        return 0;
    }

    if (!sock_send_all(s, req, n)) {
        closesocket(s);
        snprintf(err, errCap, "Failed to send request");
        return 0;
    }

    // read all into a temp buffer (toy)
    char *tmp = (char*)malloc(1024 * 256);
    int tmpCap = tmp ? (1024 * 256) : 0;
    int tmpLen = 0;

    if (!tmp) {
        closesocket(s);
        snprintf(err, errCap, "Out of memory");
        return 0;
    }

    for (;;) {
        char buf[4096];
        int r = sock_recv_some(s, buf, (int)sizeof(buf));
        if (r <= 0) break;
        if (tmpLen + r >= tmpCap) break;
        memcpy(tmp + tmpLen, buf, r);
        tmpLen += r;
    }

    closesocket(s);

    if (tmpLen <= 0) {
        free(tmp);
        snprintf(err, errCap, "No response");
        return 0;
    }
    tmp[tmpLen] = '\0';

    // split headers/body
    char *sep = strstr(tmp, "\r\n\r\n");
    if (!sep) {
        free(tmp);
        snprintf(err, errCap, "Bad HTTP response");
        return 0;
    }
    *sep = '\0';
    const char *hdrs = tmp;
    const char *body = sep + 4;

    // crude status check
    if (!strstr(hdrs, "200")) {
        // try to show some body
        char preview[256];
        strncpy(preview, body, sizeof(preview) - 1);
        preview[sizeof(preview) - 1] = '\0';
        snprintf(err, errCap, "HTTP error. Body: %.200s", preview);
        free(tmp);
        return 0;
    }

    // If chunked, decode it.
    if (header_is_chunked(hdrs)) {
        const char *p = body;
        int w = 0;
        while (*p) {
            unsigned chunkSize = 0;
            // read hex size
            while (*p && *p != '\r' && *p != '\n') {
                char c = *p++;
                int v = 0;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
                else { /* ignore */ }
                chunkSize = (chunkSize << 4) | (unsigned)v;
            }
            // skip CRLF
            while (*p == '\r' || *p == '\n') p++;
            if (chunkSize == 0) break;

            for (unsigned i = 0; i < chunkSize && *p; i++) {
                if (w < outCap - 1) out[w++] = *p;
                p++;
            }
            out[w] = '\0';
            // skip trailing CRLF after chunk data
            while (*p == '\r' || *p == '\n') p++;
        }
        free(tmp);
        return 1;
    }

    // Not chunked: if content-length exists, respect it
    int cl = parse_content_length(hdrs);
    if (cl >= 0) {
        int copy = cl;
        if (copy > outCap - 1) copy = outCap - 1;
        memcpy(out, body, copy);
        out[copy] = '\0';
        free(tmp);
        return 1;
    }

    // Otherwise copy what we have
    strncpy(out, body, outCap - 1);
    out[outCap - 1] = '\0';
    free(tmp);
    return 1;
}

// -------------------- llama request/response --------------------

static void build_llm_request_json(ToyTerm *t, const char *userText, char *out, int outCap) {
    // System prompt: force strict JSON tool-ish output
    const char *sys =
        "You are the terminal brain for a tiny raylib toy. "
        "You MUST respond with a single JSON object, no extra text. "
        "Schema:\n"
        "{\n"
        "  \"say\": string,\n"
        "  \"actions\": [\n"
        "     {\"type\":\"spawn_cube\", \"id\": int(optional), \"x\": number, \"y\": number, \"z\": number, \"size\": number, \"r\": int, \"g\": int, \"b\": int},\n"
        "     {\"type\":\"destroy_cube\", \"id\": int},\n"
        "     {\"type\":\"clear_cubes\"}\n"
        "  ]\n"
        "}\n"
        "If you are unsure, set say to ask a clarifying question and actions to [].";

    // ensure system is first message in chat buffer
    if (t->chatCount == 0) {
        chat_push(t, "system", sys);
    }

    // push user msg into chat buffer
    chat_push(t, "user", userText);

    // build JSON
    // (manual, small, not a full JSON writer)
    char escRole[32];
    char escText[2048];

    int w = 0;
    w += snprintf(out + w, outCap - w,
        "{"
        "\"model\":\"gpt-3.5-turbo\","
        "\"stream\":false,"
        "\"temperature\":0.4,"
        "\"messages\":["
    );

    for (int i = 0; i < t->chatCount && w < outCap - 1; i++) {
        json_escape(t->chat[i].role, escRole, (int)sizeof(escRole));
        json_escape(t->chat[i].text, escText, (int)sizeof(escText));

        w += snprintf(out + w, outCap - w,
            "%s{\"role\":\"%s\",\"content\":\"%s\"}",
            (i == 0) ? "" : ",", escRole, escText
        );
    }

    w += snprintf(out + w, outCap - w, "]}");
    out[outCap - 1] = '\0';
}

// Extract choices[0].message.content from OpenAI-style response JSON.
// Returns 1 ok.
static int extract_oai_content(const char *respJson, char *contentOut, int cap) {
    // find "message" then "content"
    const char *m = strstr(respJson, "\"message\"");
    if (!m) return 0;

    const char *c = strstr(m, "\"content\"");
    if (!c) return 0;

    c = strchr(c, ':');
    if (!c) return 0;
    c++;
    trim_ws(&c);

    // content can be a JSON string
    if (*c != '"') return 0;
    const char *after = json_read_string(c, contentOut, cap);
    return after ? 1 : 0;
}

// Parse the model's JSON content:
// - say string
// - actions array with spawn/destroy/clear
static void apply_model_json(ToyTerm *t, const char *modelJson) {
    // Say line
    char say[TERM_LINE_MAX];
    say[0] = '\0';

    const char *k = find_json_key(modelJson, "say");
    if (k) {
        const char *p = strchr(k, ':');
        if (p) {
            p++;
            trim_ws(&p);
            if (*p == '"') {
                char tmp[TERM_LINE_MAX];
                if (json_read_string(p, tmp, (int)sizeof(tmp))) {
                    snprintf(say, sizeof(say), "> %s", tmp);
                    hist_push(t, say);
                }
            }
        }
    }

    // Actions
    const char *a = find_json_key(modelJson, "actions");
    if (!a) return;

    const char *p = strchr(a, '[');
    if (!p) return;
    p++;

    // brute scan for objects inside array
    while (*p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        if (*p != '{') break;

        const char *objStart = p;
        // find end of object (naive brace depth)
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                if (depth == 0) { p++; break; }
            }
            p++;
        }
        int objLen = (int)(p - objStart);
        if (objLen <= 0) break;

        char obj[2048];
        if (objLen >= (int)sizeof(obj)) objLen = (int)sizeof(obj) - 1;
        memcpy(obj, objStart, objLen);
        obj[objLen] = '\0';

        // read type
        char type[64] = {0};
        const char *tk = find_json_key(obj, "type");
        if (tk) {
            const char *tp = strchr(tk, ':');
            if (tp) {
                tp++;
                trim_ws(&tp);
                if (*tp == '"') {
                    json_read_string(tp, type, (int)sizeof(type));
                }
            }
        }

        if (strcmp(type, "clear_cubes") == 0) {
            hist_push(t, LINE_OBJ_CLEAR);
        }
        else if (strcmp(type, "destroy_cube") == 0) {
            int id = -1;
            const char *ik = find_json_key(obj, "id");
            if (ik) {
                const char *ip = strchr(ik, ':');
                if (ip) id = atoi(ip + 1);
            }
            if (id >= 0) {
                char line[TERM_LINE_MAX];
                snprintf(line, sizeof(line), "%s %d", LINE_OBJ_DEL, id);
                hist_push(t, line);
            }
        }
        else if (strcmp(type, "spawn_cube") == 0) {
            int id = -1, r = 200, g = 200, b = 200;
            float x = 0, y = 0.5f, z = 6, size = 1;

            const char *ik = find_json_key(obj, "id");
            if (ik) { const char *ip = strchr(ik, ':'); if (ip) id = atoi(ip + 1); }
            if (id < 0) id = t->nextCubeId++;

            const char *kx = find_json_key(obj, "x");
            const char *ky = find_json_key(obj, "y");
            const char *kz = find_json_key(obj, "z");
            const char *ks = find_json_key(obj, "size");
            const char *kr = find_json_key(obj, "r");
            const char *kg = find_json_key(obj, "g");
            const char *kb = find_json_key(obj, "b");

            if (kx) x = (float)atof(strchr(kx, ':') + 1);
            if (ky) y = (float)atof(strchr(ky, ':') + 1);
            if (kz) z = (float)atof(strchr(kz, ':') + 1);
            if (ks) size = (float)atof(strchr(ks, ':') + 1);
            if (kr) r = atoi(strchr(kr, ':') + 1);
            if (kg) g = atoi(strchr(kg, ':') + 1);
            if (kb) b = atoi(strchr(kb, ':') + 1);

            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;

            char line[TERM_LINE_MAX];
            snprintf(line, sizeof(line),
                "%s %d %.3f %.3f %.3f %.3f %d %d %d",
                LINE_OBJ_ADD, id, x, y, z, size, r, g, b);
            hist_push(t, line);
        }

        // skip commas/space
        while (*p && *p != '{' && *p != ']') p++;
    }
}

// -------------------- public API --------------------

ToyTerm* term_create(void) {
    ToyTerm *t = (ToyTerm*)calloc(1, sizeof(ToyTerm));
    if (!t) return NULL;

    t->histCount = 0;
    t->chatCount = 0;
    t->nextCubeId = 1;

    hist_push(t, "> CONNECTED");
    hist_push(t, "> LLM TERMINAL MODE");
    hist_push(t, ">>> ");

    return t;
}

void term_destroy(ToyTerm* t) {
    free(t);
}

int term_history_count(const ToyTerm* t) {
    return t ? t->histCount : 0;
}

const char* term_history_line(const ToyTerm* t, int idx) {
    if (!t || idx < 0 || idx >= t->histCount) return NULL;
    return t->history[idx];
}

int term_run(ToyTerm* t, const char* cmdIn) {
    if (!t) return 0;
    int before = t->histCount;

    char cmd[256];
    strncpy(cmd, cmdIn ? cmdIn : "", sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    // show typed line in prompt
    char promptLine[TERM_LINE_MAX];
    snprintf(promptLine, sizeof(promptLine), ">>> %s", cmd);
    replace_last(t, promptLine);

    // trim leading whitespace
    const char *p = cmd;
    trim_ws(&p);

    // empty line -> just new prompt
    if (*p == '\0') {
        hist_push(t, ">>> ");
        return t->histCount - before;
    }

    // Optional local commands (still “chatty”, but useful)
    // (You can delete these if you want *pure* LLM.)
    if (strcmp(p, "/clear") == 0) {
        hist_push(t, LINE_OBJ_CLEAR);
        hist_push(t, ">>> ");
        return t->histCount - before;
    }

    // Call llama-server
    char reqJson[16384];
    build_llm_request_json(t, p, reqJson, (int)sizeof(reqJson));

    char respJson[1024 * 128];
    char err[256];

    if (!http_post_json(LLM_HOST, LLM_PORT, LLM_PATH, reqJson,
                        respJson, (int)sizeof(respJson),
                        err, (int)sizeof(err))) {
        char msg[TERM_LINE_MAX];
        snprintf(msg, sizeof(msg), "Error: %s", err);
        hist_push(t, msg);
        hist_push(t, ">>> ");
        return t->histCount - before;
    }

    // extract assistant content
    char content[8192];
    if (!extract_oai_content(respJson, content, (int)sizeof(content))) {
        hist_push(t, "Error: could not parse llama-server response (missing message.content)");
        hist_push(t, ">>> ");
        return t->histCount - before;
    }

    // store assistant content in chat memory so convo continues
    chat_push(t, "assistant", content);

    // interpret assistant JSON (say + actions)
    apply_model_json(t, content);

    // new prompt
    hist_push(t, ">>> ");
    return t->histCount - before;
}
