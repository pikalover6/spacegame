#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "terminal_ui.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

void termui_init(TerminalUI* t) {
    memset(t, 0, sizeof(*t));
}

void termui_clear_command(TerminalUI* t) {
    t->command[0] = '\0';
    t->cmdLen = 0;
}

void termui_push_line(TerminalUI* t, const char* line) {
    if (!line) line = "";
    if (t->histCount < HISTORY_MAX_LINES) {
        strncpy(t->history[t->histCount], line, LINE_MAX_CHARS-1);
        t->history[t->histCount][LINE_MAX_CHARS-1] = '\0';
        t->histCount++;
        return;
    }
    for (int i=1;i<HISTORY_MAX_LINES;i++) strcpy(t->history[i-1], t->history[i]);
    strncpy(t->history[HISTORY_MAX_LINES-1], line, LINE_MAX_CHARS-1);
    t->history[HISTORY_MAX_LINES-1][LINE_MAX_CHARS-1] = '\0';
}

void termui_replace_last(TerminalUI* t, const char* line) {
    if (t->histCount <= 0) return;
    strncpy(t->history[t->histCount-1], line, LINE_MAX_CHARS-1);
    t->history[t->histCount-1][LINE_MAX_CHARS-1] = '\0';
}

int termui_allowed_char(int c) {
    const char *ok = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-*/()=._\"'[]{},:<>!@#$%^&|?; ";
    return (strchr(ok, c) != NULL);
}

void termui_render(RenderTexture2D rt, Font font, const TerminalUI* t) {
    BeginTextureMode(rt);
        ClearBackground((Color){0,0,0,255});

        int start = 0;
        if (t->histCount > VISIBLE_LINES) start = t->histCount - VISIBLE_LINES;

        int fontSize = 18;
        int y = 8;
        for (int i=start; i<t->histCount; i++) {
            const char* line = t->history[i];
            if (i == t->histCount-1) {
                char composed[LINE_MAX_CHARS + COMMAND_MAX_CHARS];
                snprintf(composed, sizeof(composed), "%s%s", line, t->command);
                DrawTextEx(font, composed, (Vector2){10, (float)y}, (float)fontSize, 1.0f, GREEN);
            } else {
                DrawTextEx(font, line, (Vector2){10, (float)y}, (float)fontSize, 1.0f, GREEN);
            }
            y += fontSize + 2;
        }

        for (int sy=0; sy<rt.texture.height; sy+=4) {
            DrawLine(0, sy, rt.texture.width, sy, (Color){0,20,0,30});
        }
    EndTextureMode();
}
