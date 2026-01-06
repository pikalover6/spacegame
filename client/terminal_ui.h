#ifndef TERMINAL_UI_H
#define TERMINAL_UI_H

#include "raylib.h"

#define HISTORY_MAX_LINES 256
#define LINE_MAX_CHARS    256
#define COMMAND_MAX_CHARS 256
#define VISIBLE_LINES     12

typedef struct {
    char history[HISTORY_MAX_LINES][LINE_MAX_CHARS];
    int  histCount;

    char command[COMMAND_MAX_CHARS];
    int  cmdLen;
} TerminalUI;

void termui_init(TerminalUI* t);
void termui_clear_command(TerminalUI* t);
void termui_push_line(TerminalUI* t, const char* line);
void termui_replace_last(TerminalUI* t, const char* line);

void termui_render(RenderTexture2D rt, Font font, const TerminalUI* t);

int  termui_allowed_char(int c);

#endif
