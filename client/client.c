#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "raylib.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "net.h"
#include "terminal_ui.h"
#include "psx_shader.h"
#include "../common/protocol.h"

typedef struct {
    float x, y, z;
    float yaw, pitch;
} PlayerState;

typedef struct {
    NetClient net;
    TerminalUI term;
    PlayerState ps;

    int focused;
    int paused;

    int haveHistory;
    int expectHist;
    int gotHist;

    // prediction
    int haveState;
    Vector3 predPos;
    float predYaw;
    float predPitch;
} ClientState;

static float Snap(float v, float step) {
    return floorf(v / step + 0.5f) * step;
}

static Vector3 SnapV3(Vector3 v, float step) {
    v.x = Snap(v.x, step);
    v.y = Snap(v.y, step);
    v.z = Snap(v.z, step);
    return v;
}

static void SetMouseCaptured(int captured) {
    if (captured) {
        DisableCursor();
        int w = GetScreenWidth();
        int h = GetScreenHeight();
        SetMousePosition(w / 2, h / 2);
        (void)GetMouseDelta();
    } else {
        EnableCursor();
    }
}

static void on_server_line(const char* line, void* ud) {
    ClientState* cs = (ClientState*)ud;

    if (strncmp(line, "HIST ", 5) == 0) {
        cs->expectHist = atoi(line + 5);
        cs->gotHist = 0;
        cs->term.histCount = 0;
        cs->haveHistory = 1;
    }
    else if (strncmp(line, "LINE ", 5) == 0) {
        termui_push_line(&cs->term, line + 5);
        cs->gotHist++;
    }
    else if (strncmp(line, "STATE ", 6) == 0) {
        sscanf(line + 6, "%f %f %f %f %f",
            &cs->ps.x, &cs->ps.y, &cs->ps.z,
            &cs->ps.yaw, &cs->ps.pitch);

        if (!cs->haveState) {
            cs->haveState = 1;
            cs->predPos = (Vector3){ cs->ps.x, cs->ps.y, cs->ps.z };
            cs->predYaw = cs->ps.yaw;
            cs->predPitch = cs->ps.pitch;
        }
    }
}

static int ray_hit_box(Camera3D cam, BoundingBox box) {
    Ray ray = GetMouseRay(GetMousePosition(), cam);
    RayCollision hit = GetRayCollisionBox(ray, box);
    return hit.hit;
}

int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "0x10c Prototype - Client");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    if (!net_init()) return 1;

    ClientState cs = { 0 };
    termui_init(&cs.term);

    if (!net_connect(&cs.net, "127.0.0.1", 27015)) return 1;
    net_sendf(&cs.net, "HELLO\n");

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 0.0f, 1.6f, 2.0f };
    camera.target   = (Vector3){ 0.0f, 1.6f, 3.0f };
    camera.up       = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy     = 70.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    Vector3 deskPos  = { 0, 0.5f, 7 };
    Vector3 deskSize = { 3, 1, 1.5f };
    Vector3 monPos   = { deskPos.x, deskPos.y + 0.8f, deskPos.z };
    Vector3 monSize  = { 0.8f, 0.6f, 0.05f };

    BoundingBox monBox = {
        { monPos.x - monSize.x / 2, monPos.y - monSize.y / 2, monPos.z - monSize.z / 2 },
        { monPos.x + monSize.x / 2, monPos.y + monSize.y / 2, monPos.z + monSize.z / 2 }
    };

    RenderTexture2D termRT = LoadRenderTexture(512, 256);
    RenderTexture2D sceneRT = LoadRenderTexture(320, 180);
    SetTextureFilter(sceneRT.texture, TEXTURE_FILTER_POINT);
    Shader psxShader = LoadPsxShader();

    SetMouseCaptured(1);

    while (!WindowShouldClose()) {
        if (!net_poll_lines(&cs.net, on_server_line, &cs)) break;

        if (IsKeyPressed(KEY_ESCAPE)) {
            if (cs.focused) {
                cs.focused = 0;
                SetMouseCaptured(!cs.paused);
            } else {
                cs.paused = !cs.paused;
                SetMouseCaptured(!cs.paused && !cs.focused);
            }
        }

        if (IsKeyPressed(KEY_Q) && !cs.focused) break;

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !cs.paused && !cs.focused) {
            if (ray_hit_box(camera, monBox)) {
                cs.focused = 1;
                SetMouseCaptured(1);
            }
        }

        if (cs.focused) {
            if (IsKeyPressed(KEY_ENTER)) {
                net_sendf(&cs.net, "CMD %s\n", cs.term.command);
                termui_clear_command(&cs.term);
            }

            if (IsKeyPressed(KEY_BACKSPACE) && cs.term.cmdLen > 0) {
                cs.term.command[--cs.term.cmdLen] = 0;
            }

            int ch = GetCharPressed();
            while (ch > 0) {
                if (cs.term.cmdLen < COMMAND_MAX_CHARS - 1 &&
                    termui_allowed_char(ch)) {
                    cs.term.command[cs.term.cmdLen++] = (char)ch;
                    cs.term.command[cs.term.cmdLen] = 0;
                }
                ch = GetCharPressed();
            }
        }

        float dt = GetFrameTime();

        if (!cs.haveState) {
            cs.predPos = camera.position;
            cs.predYaw = 0;
            cs.predPitch = 0;
        }

        if (!cs.paused && !cs.focused) {
            float fwd = IsKeyDown(KEY_W) - IsKeyDown(KEY_S);
            float right = IsKeyDown(KEY_D) - IsKeyDown(KEY_A);
            // Jumping disabled for now
            float up = 0; // IsKeyDown(KEY_SPACE) - IsKeyDown(KEY_LEFT_CONTROL);

            Vector2 md = GetMouseDelta();
            float sens = 0.0025f;

            float yawDelta = -md.x * sens;
            float pitchDelta = -md.y * sens;

            cs.predYaw += yawDelta;
            cs.predPitch += pitchDelta;

            float limit = 1.55f;
            if (cs.predPitch > limit) cs.predPitch = limit;
            if (cs.predPitch < -limit) cs.predPitch = -limit;

            float cy = cosf(cs.predYaw), sy = sinf(cs.predYaw);
            Vector3 forward = { sy, 0, cy };
            Vector3 rightV = { -cy, 0, sy };

            Vector3 wish = {
                forward.x * fwd + rightV.x * right,
                up,
                forward.z * fwd + rightV.z * right
            };

            float len = sqrtf(wish.x*wish.x + wish.y*wish.y + wish.z*wish.z);
            if (len > 0.001f) {
                wish.x /= len; wish.y /= len; wish.z /= len;
            }

            float speed = 4.5f;
            cs.predPos.x += wish.x * speed * dt;
            cs.predPos.y += wish.y * speed * dt;
            cs.predPos.z += wish.z * speed * dt;

            net_sendf(&cs.net,
                "INPUT %.3f %.3f %.3f %.6f %.6f %.6f\n",
                fwd, right, up, yawDelta, pitchDelta, dt);
        }

        if (cs.haveState) {
            float a = 1.0f - expf(-12.0f * dt);
            cs.predPos.x += (cs.ps.x - cs.predPos.x) * a;
            cs.predPos.y += (cs.ps.y - cs.predPos.y) * a;
            cs.predPos.z += (cs.ps.z - cs.predPos.z) * a;
            cs.predYaw   += (cs.ps.yaw - cs.predYaw) * a;
            cs.predPitch += (cs.ps.pitch - cs.predPitch) * a;
        }

        float cy = cosf(cs.predYaw), sy = sinf(cs.predYaw);
        float cp = cosf(cs.predPitch), sp = sinf(cs.predPitch);

        camera.position = cs.predPos;
        camera.target = (Vector3){
            cs.predPos.x + sy * cp,
            cs.predPos.y + sp,
            cs.predPos.z + cy * cp
        };

        camera.position = SnapV3(camera.position, 1.0f / 64.0f);
        camera.target   = SnapV3(camera.target,   1.0f / 64.0f);

        termui_render(termRT, GetFontDefault(), &cs.term);

        BeginTextureMode(sceneRT);
            ClearBackground((Color){ 10, 10, 12, 255 });
            BeginMode3D(camera);
                DrawGrid(20, 1.0f);
                DrawCube(deskPos, deskSize.x, deskSize.y, deskSize.z, DARKGRAY);

                Vector3 screenPos = (Vector3){ monPos.x, monPos.y, monPos.z - (monSize.z/2 + 0.001f) };
                Vector2 screenSize = (Vector2){ monSize.x * 0.95f, monSize.y * 0.90f };

                Rectangle srcTerm = (Rectangle){ 0, 0, (float)termRT.texture.width, (float)-termRT.texture.height };
                DrawBillboardRec(camera, termRT.texture, srcTerm, screenPos, screenSize, WHITE);
            EndMode3D();
        EndTextureMode();

        BeginDrawing();
            ClearBackground(BLACK);
            DrawTexturePro(sceneRT.texture,
                (Rectangle){ 0,0,320,-180 },
                (Rectangle){ 0,0,(float)GetScreenWidth(),(float)GetScreenHeight() },
                (Vector2){ 0,0 }, 0, WHITE);
        EndDrawing();
    }

    net_close(&cs.net);
    net_shutdown();
    CloseWindow();
    return 0;
}
