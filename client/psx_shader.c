#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "psx_shader.h"

static const char *PSX_FS =
"#version 330\n"
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"
"out vec4 finalColor;\n"
"uniform sampler2D texture0;\n"
"uniform vec4 colDiffuse;\n"
"int bayer4x4(ivec2 p){\n"
"    int x = p.x & 3;\n"
"    int y = p.y & 3;\n"
"    int m[16] = int[16](\n"
"        0,  8,  2, 10,\n"
"       12,  4, 14,  6,\n"
"        3, 11,  1,  9,\n"
"       15,  7, 13,  5\n"
"    );\n"
"    return m[y*4 + x];\n"
"}\n"
"vec3 quantize(vec3 c, float levels){\n"
"    return floor(c*levels + 0.5)/levels;\n"
"}\n"
"void main(){\n"
"    vec4 tex = texture(texture0, fragTexCoord) * colDiffuse * fragColor;\n"
"    ivec2 p = ivec2(gl_FragCoord.xy);\n"
"    float d = (float(bayer4x4(p)) / 16.0 - 0.5) * (1.0/255.0) * 48.0;\n"
"    vec3 c = tex.rgb + d;\n"
"    c = quantize(clamp(c, 0.0, 1.0), 32.0);\n"
"    finalColor = vec4(c, tex.a);\n"
"}\n";

Shader LoadPsxShader(void) {
    return LoadShaderFromMemory(0, PSX_FS);
}
