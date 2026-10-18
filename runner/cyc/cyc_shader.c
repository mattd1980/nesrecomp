/*
 * cyc_shader.c - the windowed host's optional GLSL pass (cyc_shader.h).
 *
 * Every GL entry point comes from SDL_GL_GetProcAddress, so the host links no
 * GL library and a build without a shader never touches GL.
 */
#include "cyc_shader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <SDL_opengl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char GLcharT;
#define CYC_GL_FN(ret, name, args) typedef ret (APIENTRY *name##_fn) args;
#define CYC_GL_FNS(X) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(GLboolean, glIsEnabled, (GLenum)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glEnableClientState, (GLenum)) \
    X(void, glDisableClientState, (GLenum)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLcharT *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLcharT *)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLcharT *)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLcharT *)) \
    X(GLint, glGetAttribLocation, (GLuint, const GLcharT *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint))
CYC_GL_FNS(CYC_GL_FN)
#undef CYC_GL_FN

typedef struct {
#define CYC_GL_FIELD(ret, name, args) name##_fn name;
    CYC_GL_FNS(CYC_GL_FIELD)
#undef CYC_GL_FIELD
} GlApi;

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

struct CycShader {
    char   path[1024];   /* the .glsl actually compiled */
    char  *source;
    bool   compiled, failed;
    char   err[512];
    GlApi  gl;
    GLuint prog;
    GLint  a_vertex, a_texcoord, a_color;
    GLint  u_mvp, u_texture, u_texture_size, u_input_size, u_output_size, u_frame_count, u_frame_direction;
};

/* ---- files ---- */

static bool file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static bool is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

/* As written, against the working directory (the launcher stores a pick
 * relative to it), else against the executable's directory. */
static bool resolve(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    if (is_absolute(path) || file_exists(out)) return file_exists(out);
    char *base = SDL_GetBasePath();
    if (base) {
        snprintf(out, n, "%s%s", base, path);
        SDL_free(base);
        if (file_exists(out)) return true;
    }
    snprintf(out, n, "%s", path);
    return false;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = len >= 0 && len < (16L << 20) ? (char *)malloc((size_t)len + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
    if (buf) buf[len] = 0;
    fclose(f);
    return buf;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* A .glslp names its passes; one pass is a .glsl beside the preset. */
static bool preset_pass(const char *preset, char *out, size_t n, char *err, size_t en)
{
    char *text = read_file(preset);
    if (!text) { snprintf(err, en, "cannot read %s", preset); return false; }
    int passes = -1;
    char shader0[1024] = "";
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *t = trim(line), *eq = strchr(t, '=');
        if (!eq || *t == '#') continue;
        *eq = 0;
        char *key = trim(t), *val = trim(eq + 1);
        size_t vl = strlen(val);
        if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') { val[vl - 1] = 0; ++val; }
        if (!strcmp(key, "shaders")) passes = atoi(val);
        else if (!strcmp(key, "shader0")) snprintf(shader0, sizeof(shader0), "%s", val);
    }
    free(text);
    if (passes != 1 || !shader0[0]) {
        snprintf(err, en, "%s: only one-pass presets are supported (shaders = %d)", preset, passes);
        return false;
    }
    if (is_absolute(shader0)) { snprintf(out, n, "%s", shader0); return true; }
    snprintf(out, n, "%s", preset);
    char *cut = strrchr(out, '/'), *bcut = strrchr(out, '\\');
    if (bcut > cut) cut = bcut;
    if (cut) snprintf(cut + 1, n - (size_t)(cut + 1 - out), "%s", shader0);
    else snprintf(out, n, "%s", shader0);
    return true;
}

static void normalize(char *p)
{
    for (; *p; ++p) *p = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
}

bool cyc_shader_same(const char *a, const char *b)
{
    if (!a || !b || !*a || !*b) return (!a || !*a) && (!b || !*b);
    char ra[1024], rb[1024];
    resolve(a, ra, sizeof(ra));
    resolve(b, rb, sizeof(rb));
    normalize(ra);
    normalize(rb);
    return strcmp(ra, rb) == 0;
}

/* ---- the bundled list: <exe dir>/assets/shaders, as the launcher lists it
 * (top level; names starting with _ or . are private) ---- */

static bool listed_name(const char *name)
{
    size_t len = strlen(name);
    if (!len || name[0] == '_' || name[0] == '.') return false;
    return (len > 5 && SDL_strcasecmp(name + len - 5, ".glsl") == 0) ||
           (len > 6 && SDL_strcasecmp(name + len - 6, ".glslp") == 0);
}

static void add_entry(CycShaderEntry *out, int max, int *n, const char *name)
{
    if (*n >= max || !listed_name(name)) return;
    CycShaderEntry *e = &out[(*n)++];
    snprintf(e->path, sizeof(e->path), "assets/shaders/%s", name);
    /* the launcher's label: no extension, - and _ as spaces, words capitalized */
    snprintf(e->label, sizeof(e->label), "%s", name);
    char *dot = strrchr(e->label, '.');
    if (dot) *dot = 0;
    bool cap = true;
    for (char *c = e->label; *c; ++c) {
        if (*c == '-' || *c == '_') *c = ' ';
        if (*c == ' ') cap = true;
        else if (cap) { *c = (char)toupper((unsigned char)*c); cap = false; }
    }
}

static int by_label(const void *a, const void *b)
{
    return strcmp(((const CycShaderEntry *)a)->label, ((const CycShaderEntry *)b)->label);
}

int cyc_shader_list(CycShaderEntry *out, int max)
{
    int n = 0;
    char *base = SDL_GetBasePath();
    char dir[1024];
    snprintf(dir, sizeof(dir), "%sassets/shaders", base ? base : "");
    SDL_free(base);
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) add_entry(out, max, &n, fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) add_entry(out, max, &n, e->d_name);
        closedir(d);
    }
#endif
    qsort(out, (size_t)n, sizeof(out[0]), by_label);
    return n;
}

/* ---- loading ---- */

void cyc_shader_request_renderer(void)
{
    /* SDL_RENDER_DRIVER from the environment still wins */
    if (!SDL_GetHint(SDL_HINT_RENDER_DRIVER)) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
}

CycShader *cyc_shader_load(SDL_Renderer *ren, const char *path, char *err, size_t n)
{
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(ren, &info) != 0 || !info.name || strcmp(info.name, "opengl") != 0) {
        snprintf(err, n, "shaders need the OpenGL renderer (this window has %s)",
                 info.name ? info.name : "an unknown one");
        return NULL;
    }
    char file[1024];
    if (!resolve(path, file, sizeof(file))) { snprintf(err, n, "%s not found", path); return NULL; }
    size_t len = strlen(file);
    if (len > 6 && SDL_strcasecmp(file + len - 6, ".glslp") == 0) {
        char pass[1024];
        if (!preset_pass(file, pass, sizeof(pass), err, n)) return NULL;
        if (!file_exists(pass)) { snprintf(err, n, "%s not found", pass); return NULL; }
        snprintf(file, sizeof(file), "%s", pass);
    }
    CycShader *sh = (CycShader *)calloc(1, sizeof(*sh));
    if (!sh) { snprintf(err, n, "out of memory"); return NULL; }
    snprintf(sh->path, sizeof(sh->path), "%s", file);
    sh->source = read_file(file);
    if (!sh->source) { snprintf(err, n, "cannot read %s", file); free(sh); return NULL; }
    return sh;
}

/* The source for one stage: the shader's own #version (else 120) first, then
 * the stage's define. */
static char *stage_source(const char *src, const char *stage)
{
    char version[64] = "#version 120";
    size_t len = strlen(src);
    char *body = (char *)malloc(len + 1);
    if (!body) return NULL;
    memcpy(body, src, len + 1);
    for (char *line = body; line && *line;) {
        char *t = line;
        while (*t == ' ' || *t == '\t') ++t;
        if (!strncmp(t, "#version", 8)) {
            size_t vl = strcspn(t, "\r\n");
            snprintf(version, sizeof(version), "%.*s", (int)(vl < sizeof(version) - 1 ? vl : sizeof(version) - 1), t);
            t[0] = '/'; t[1] = '/';   /* the line stays, as a comment */
            break;
        }
        line = strchr(line, '\n');
        if (line) ++line;
    }
    size_t need = strlen(version) + strlen(stage) + len + 32;
    char *out = (char *)malloc(need);
    if (out) snprintf(out, need, "%s\n#define %s\n%s", version, stage, body);
    free(body);
    return out;
}

static GLuint compile_stage(CycShader *sh, GLenum type, const char *stage)
{
    char *src = stage_source(sh->source, stage);
    if (!src) { snprintf(sh->err, sizeof(sh->err), "out of memory"); return 0; }
    GLuint s = sh->gl.glCreateShader(type);
    const GLcharT *srcs[1] = { src };
    sh->gl.glShaderSource(s, 1, srcs, NULL);
    sh->gl.glCompileShader(s);
    free(src);
    GLint ok = 0;
    sh->gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[400] = "";
        sh->gl.glGetShaderInfoLog(s, sizeof(log), NULL, log);
        snprintf(sh->err, sizeof(sh->err), "%s stage: %s", stage, trim(log));
        sh->gl.glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool compile(CycShader *sh)
{
#define CYC_GL_LOAD(ret, name, args) \
    if (!(sh->gl.name = (name##_fn)SDL_GL_GetProcAddress(#name))) { \
        snprintf(sh->err, sizeof(sh->err), "the OpenGL driver has no %s", #name); \
        return false; \
    }
    CYC_GL_FNS(CYC_GL_LOAD)
#undef CYC_GL_LOAD
    GLuint vs = compile_stage(sh, GL_VERTEX_SHADER, "VERTEX");
    if (!vs) return false;
    GLuint fs = compile_stage(sh, GL_FRAGMENT_SHADER, "FRAGMENT");
    if (!fs) { sh->gl.glDeleteShader(vs); return false; }
    sh->prog = sh->gl.glCreateProgram();
    sh->gl.glAttachShader(sh->prog, vs);
    sh->gl.glAttachShader(sh->prog, fs);
    sh->gl.glLinkProgram(sh->prog);
    sh->gl.glDeleteShader(vs);
    sh->gl.glDeleteShader(fs);
    GLint ok = 0;
    sh->gl.glGetProgramiv(sh->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[400] = "";
        sh->gl.glGetProgramInfoLog(sh->prog, sizeof(log), NULL, log);
        snprintf(sh->err, sizeof(sh->err), "link: %s", trim(log));
        sh->gl.glDeleteProgram(sh->prog);
        sh->prog = 0;
        return false;
    }
    sh->a_vertex = sh->gl.glGetAttribLocation(sh->prog, "VertexCoord");
    sh->a_texcoord = sh->gl.glGetAttribLocation(sh->prog, "TexCoord");
    sh->a_color = sh->gl.glGetAttribLocation(sh->prog, "COLOR");
    if (sh->a_vertex < 0) {
        snprintf(sh->err, sizeof(sh->err), "no VertexCoord attribute (not a RetroArch-style .glsl?)");
        return false;
    }
    sh->u_mvp = sh->gl.glGetUniformLocation(sh->prog, "MVPMatrix");
    sh->u_texture = sh->gl.glGetUniformLocation(sh->prog, "Texture");
    sh->u_texture_size = sh->gl.glGetUniformLocation(sh->prog, "TextureSize");
    sh->u_input_size = sh->gl.glGetUniformLocation(sh->prog, "InputSize");
    sh->u_output_size = sh->gl.glGetUniformLocation(sh->prog, "OutputSize");
    sh->u_frame_count = sh->gl.glGetUniformLocation(sh->prog, "FrameCount");
    sh->u_frame_direction = sh->gl.glGetUniformLocation(sh->prog, "FrameDirection");
    printf("[cyc shader] %s\n", sh->path);
    fflush(stdout);
    return true;
}

/* ---- drawing ---- */

/* The picture's rectangle in drawable pixels, GL's bottom-left origin: SDL2's
 * logical-size viewport (centered, integer when asked) with the picture at
 * logical (0,0). */
static void picture_rect(SDL_Renderer *ren, int w, int h, GLint r[4])
{
    int ow = 0, oh = 0, lw = 0, lh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    if (lw <= 0 || lh <= 0) { lw = ow; lh = oh; }
    float sx = (float)ow / (float)lw, sy = (float)oh / (float)lh;
    float sc = sx < sy ? sx : sy;
    if (SDL_RenderGetIntegerScale(ren)) {
        sc = SDL_floorf(sc);
        if (sc < 1.0f) sc = 1.0f;
    }
    int vw = (int)SDL_ceilf((float)lw * sc), vh = (int)SDL_ceilf((float)lh * sc);
    int vx = (ow - vw) / 2, vy = (oh - vh) / 2;
    r[2] = (GLint)SDL_ceilf((float)w * sc);
    r[3] = (GLint)SDL_ceilf((float)h * sc);
    r[0] = vx;
    r[1] = oh - (vy + r[3]);
}

bool cyc_shader_draw(CycShader *sh, SDL_Renderer *ren, SDL_Texture *tex, int w, int h, long frame)
{
    if (!sh || sh->failed) return false;
    SDL_RenderFlush(ren);
    float tw = 0, th = 0;
    if (SDL_GL_BindTexture(tex, &tw, &th) != 0) {
        snprintf(sh->err, sizeof(sh->err), "cannot bind the picture: %s", SDL_GetError());
        sh->failed = true;
        return false;
    }
    if (!sh->compiled) {
        sh->compiled = true;
        if (tw > 1.001f || th > 1.001f) {
            snprintf(sh->err, sizeof(sh->err), "the renderer uses rectangle textures");
            sh->failed = true;
        } else if (!compile(sh)) {
            sh->failed = true;
        }
        if (sh->failed) { SDL_GL_UnbindTexture(tex); return false; }
    }
    const GlApi *gl = &sh->gl;

    /* what the SDL renderer believes is current */
    GLint prog = 0, viewport[4], active = GL_TEXTURE0, buffer = 0;
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    gl->glGetIntegerv(GL_VIEWPORT, viewport);
    gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    GLboolean blend = gl->glIsEnabled(GL_BLEND), scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
    GLboolean va = gl->glIsEnabled(GL_VERTEX_ARRAY), ta = gl->glIsEnabled(GL_TEXTURE_COORD_ARRAY),
              ca = gl->glIsEnabled(GL_COLOR_ARRAY);

    GLint r[4];
    picture_rect(ren, w, h, r);
    gl->glViewport(r[0], r[1], r[2], r[3]);
    if (blend) gl->glDisable(GL_BLEND);
    if (scissor) gl->glDisable(GL_SCISSOR_TEST);
    if (va) gl->glDisableClientState(GL_VERTEX_ARRAY);
    if (ta) gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca) gl->glDisableClientState(GL_COLOR_ARRAY);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glUseProgram(sh->prog);

    /* RetroArch's quad: VertexCoord in 0..1 through an ortho MVP; the
     * picture's first row (texture row 0) at the top. */
    static const GLfloat mvp[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
    static const GLfloat vert[16] = { 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1 };
    static const GLfloat texc[16] = { 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
    static const GLfloat color[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    if (sh->u_mvp >= 0) gl->glUniformMatrix4fv(sh->u_mvp, 1, GL_FALSE, mvp);
    if (sh->u_texture >= 0) gl->glUniform1i(sh->u_texture, active - GL_TEXTURE0);
    if (sh->u_texture_size >= 0) gl->glUniform2f(sh->u_texture_size, (GLfloat)w, (GLfloat)h);
    if (sh->u_input_size >= 0) gl->glUniform2f(sh->u_input_size, (GLfloat)w, (GLfloat)h);
    if (sh->u_output_size >= 0) gl->glUniform2f(sh->u_output_size, (GLfloat)r[2], (GLfloat)r[3]);
    if (sh->u_frame_count >= 0) gl->glUniform1i(sh->u_frame_count, (GLint)frame);
    if (sh->u_frame_direction >= 0) gl->glUniform1i(sh->u_frame_direction, 1);
    const GLint attr[3] = { sh->a_vertex, sh->a_texcoord, sh->a_color };
    const GLfloat *data[3] = { vert, texc, color };
    for (int i = 0; i < 3; ++i) {
        if (attr[i] < 0) continue;
        gl->glEnableVertexAttribArray((GLuint)attr[i]);
        gl->glVertexAttribPointer((GLuint)attr[i], 4, GL_FLOAT, GL_FALSE, 0, data[i]);
    }
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    for (int i = 0; i < 3; ++i)
        if (attr[i] >= 0) gl->glDisableVertexAttribArray((GLuint)attr[i]);

    gl->glUseProgram((GLuint)prog);
    gl->glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
    if (va) gl->glEnableClientState(GL_VERTEX_ARRAY);
    if (ta) gl->glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca) gl->glEnableClientState(GL_COLOR_ARRAY);
    if (scissor) gl->glEnable(GL_SCISSOR_TEST);
    if (blend) gl->glEnable(GL_BLEND);
    gl->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    SDL_GL_UnbindTexture(tex);
    return true;
}

const char *cyc_shader_error(const CycShader *sh)
{
    return sh && sh->failed ? sh->err : NULL;
}

void cyc_shader_free(CycShader *sh, SDL_Renderer *ren, SDL_Texture *tex)
{
    if (!sh) return;
    /* binding a texture makes the renderer's context current */
    if (sh->prog && ren && tex) {
        SDL_RenderFlush(ren);
        if (SDL_GL_BindTexture(tex, NULL, NULL) == 0) {
            sh->gl.glDeleteProgram(sh->prog);
            SDL_GL_UnbindTexture(tex);
        }
    }
    free(sh->source);
    free(sh);
}
