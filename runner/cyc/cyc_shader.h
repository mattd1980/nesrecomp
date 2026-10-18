/*
 * cyc_shader.h - an optional GLSL pass for the windowed host's picture
 * (cyc_sdl.c): config.ini [Display] Shader, picked in recomp-ui's launcher or,
 * live, in the runtime menu's Graphics > Shader (cyc_ui_menu.c).
 *
 * The shader format is RetroArch's single-file GLSL (.glsl, compiled once with
 * VERTEX and once with FRAGMENT defined; attributes VertexCoord / TexCoord /
 * COLOR, uniforms MVPMatrix, Texture, TextureSize, InputSize, OutputSize,
 * FrameCount, FrameDirection; #pragma parameter defaults), or a .glslp preset
 * with exactly one pass. Bundled shaders live in <exe dir>/assets/shaders.
 *
 * The pass draws only the picture, through the SDL renderer's own OpenGL
 * context: the menu, toast and crosshair drawn after it stay unfiltered.
 * Anything the SDL renderer caches (program, viewport, blend, scissor, client
 * arrays, the texture binding) is restored before it returns.
 */
#pragma once
#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CycShader CycShader;

/* Before SDL_CreateRenderer: shaders need the OpenGL renderer, so the window
 * takes it from the start and the menu can switch shaders live
 * (SDL_RENDER_DRIVER in the environment overrides). */
void cyc_shader_request_renderer(void);

/* The bundled shaders, sorted by label: <exe dir>/assets/shaders top level,
 * as recomp-ui's launcher lists them. path is "assets/shaders/<file>". */
typedef struct { char label[64]; char path[512]; } CycShaderEntry;
int cyc_shader_list(CycShaderEntry *out, int max);
/* Whether two settings name the same file (resolved like cyc_shader_load;
 * two empty paths are the same "none"). */
bool cyc_shader_same(const char *a, const char *b);

/* Reads the shader (a .glsl, or a one-pass .glslp) for `ren`, which must be the
 * SDL "opengl" renderer. NULL and a reason in err when it cannot be used.
 * Compilation waits for the first cyc_shader_draw. */
CycShader *cyc_shader_load(SDL_Renderer *ren, const char *path, char *err, size_t n);

/* Draws `tex` (w x h, the picture at logical (0,0)) through the shader into
 * the picture's place in the window. False draws nothing: the caller copies
 * the picture as usual. After a failure cyc_shader_error() says why and every
 * later call returns false. */
bool cyc_shader_draw(CycShader *sh, SDL_Renderer *ren, SDL_Texture *tex, int w, int h, long frame);
const char *cyc_shader_error(const CycShader *sh);   /* NULL while it works */

/* Deletes the GL program through ren's context (tex: any of its textures). */
void cyc_shader_free(CycShader *sh, SDL_Renderer *ren, SDL_Texture *tex);

#ifdef __cplusplus
}
#endif
