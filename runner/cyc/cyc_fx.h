/*
 * cyc_fx.h - presentation effects a game draws over its own picture.
 *
 * Particles, expanding rings and short-lived lights in the game's world
 * coordinates (whatever unit the caller picks; pixels with x growing to the
 * right and y down). A game spawns them when it sees an event in RAM, calls
 * cyc_fx_advance once per emulated frame (frame_end, cyc_host_extras.h) and
 * cyc_fx_draw from its present callback, passing where world (0, 0) lands in
 * the picture. A paused game or an open menu keeps them still; a frame jump
 * (state load, rewind) drops them. Nothing here touches the machine: game
 * logic, save states and replays are unchanged.
 *
 * Looks:
 *   GLOW   additive soft disc (light, sparks' heads, embers)
 *   DUST   blended disc that grows as it fades (smoke, puffs)
 *   SPARK  additive streak along its velocity
 *   CHUNK  blended square, for debris
 * A particle with fire set ramps white -> yellow -> orange -> red over its
 * life instead of keeping its colour.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CYC_FX_GLOW, CYC_FX_DUST, CYC_FX_SPARK, CYC_FX_CHUNK } CycFxLook;

typedef struct {
    float x, y, vx, vy;     /* position, velocity per frame */
    float gravity, drag;    /* vy += gravity, v *= 1 - drag, per frame */
    float life, size, grow; /* frames, radius (pixels), radius change per frame */
    float alpha;            /* 0..1 at birth, fades to 0 over life */
    uint8_t r, g, b, fire;
    CycFxLook look;
} CycFxParticle;

void  cyc_fx_reset(void);
/* Scales every effect's opacity and light (1 = as spawned). */
void  cyc_fx_set_strength(float strength);
/* Ages everything one step per new frame number; a backwards or long jump drops it all. */
void  cyc_fx_advance(uint64_t frame);
float cyc_fx_random(void);                       /* 0..1, deterministic from reset */

void  cyc_fx_particle(const CycFxParticle *p);
/* count particles like *base, thrown in every direction at speed * (0.4..1),
 * base->life and size varied by +-30%. */
void  cyc_fx_burst(const CycFxParticle *base, int count, float speed);
/* A thin ring that grows from radius 0 to `radius` over `life` frames. */
void  cyc_fx_ring(float x, float y, float radius, float life, float alpha, uint8_t r, uint8_t g, uint8_t b);
/* A coloured light that decays over `life` frames; `amount` ~0.5 is bright. */
void  cyc_fx_light(float x, float y, float radius, float life, float amount, float r, float g, float b);

/* Draw onto ARGB8888 `pic` (w x h). World (x, y) is drawn at
 * (x + origin_x, y + origin_y); rows above clip_top are left alone. */
void  cyc_fx_draw(uint32_t *pic, int w, int h, float origin_x, float origin_y, int clip_top);
int   cyc_fx_active(void);                       /* anything left to draw */

#ifdef __cplusplus
}
#endif
