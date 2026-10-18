/*
 * cyc_touch.h - on-screen touch controls for the windowed cycle host.
 *
 * An 8-way D-pad on the left, B and A on the right (plus an A+B zone between
 * them), Select and Start along the bottom and a Menu button at the top
 * right, laid out from the window's size in landscape or portrait. Every
 * finger is tracked (hold Right and press A), buttons light up while held.
 * They add player 1's buttons and the Menu shortcut (cyc_input.h); nothing
 * else changes.
 *
 * On by default on Android, off elsewhere; NESRECOMP_TOUCH=1 / 0 overrides
 * (a desktop's mouse then drives them through SDL's mouse-to-touch events).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Window SDL_Window;

/* After SDL_Init and before the renderer: fingers are read by an SDL event
 * watch ahead of the renderer's, which would map them to the picture. */
void cyc_touch_init(void);
void cyc_touch_set_window(SDL_Window *window);   /* the window fingers are measured on */
bool cyc_touch_enabled(void);
uint8_t cyc_touch_buttons(void);        /* NES bits, A = 0x80 ... Right = 0x01 */
bool cyc_touch_menu_held(void);
void cyc_touch_release_all(void);
/* Draw over everything presented so far, in window pixels (the renderer's
 * logical size, viewport and scale are restored). */
void cyc_touch_draw(SDL_Renderer *ren);

#ifdef __cplusplus
}
#endif
