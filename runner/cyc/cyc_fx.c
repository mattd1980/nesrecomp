/* cyc_fx.c - presentation effects (cyc_fx.h). */
#include "cyc_fx.h"

#include <math.h>

#define MAX_PARTS  1024
#define MAX_RINGS  24
#define MAX_LIGHTS 24

typedef struct { CycFxParticle p; float age; } Part;
typedef struct { float x, y, radius, life, alpha, age; uint8_t r, g, b; } Ring;
typedef struct { float x, y, radius, life, amount, r, g, b, age; } Light;

static Part  s_parts[MAX_PARTS];
static Ring  s_rings[MAX_RINGS];
static Light s_lights[MAX_LIGHTS];
static int   s_nparts, s_nrings, s_nlights;
static float s_strength = 1.0f;
static uint64_t s_frame;
static int   s_have_frame;
static uint32_t s_rng = 0x6d2b79f5u;

void cyc_fx_reset(void)
{
    s_nparts = s_nrings = s_nlights = 0;
    s_have_frame = 0;
    s_rng = 0x6d2b79f5u;
}

void cyc_fx_set_strength(float strength) { s_strength = strength < 0.0f ? 0.0f : strength; }

int cyc_fx_active(void) { return s_nparts || s_nrings || s_nlights; }

float cyc_fx_random(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / 16777216.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t pack(float r, float g, float b, uint32_t a)
{
    return a | ((uint32_t)clampf(r, 0, 255) << 16) | ((uint32_t)clampf(g, 0, 255) << 8) | (uint32_t)clampf(b, 0, 255);
}

void cyc_fx_advance(uint64_t frame)
{
    if (s_have_frame && frame == s_frame) return;
    if (s_have_frame && (frame < s_frame || frame - s_frame > 4)) s_nparts = s_nrings = s_nlights = 0;
    s_frame = frame;
    s_have_frame = 1;
    int k = 0;
    for (int i = 0; i < s_nparts; ++i) {
        Part q = s_parts[i];
        if ((q.age += 1.0f) >= q.p.life) continue;
        q.p.vy += q.p.gravity;
        q.p.vx *= 1.0f - q.p.drag; q.p.vy *= 1.0f - q.p.drag;
        q.p.x += q.p.vx; q.p.y += q.p.vy;
        q.p.size += q.p.grow;
        if (q.p.size <= 0.05f) continue;
        s_parts[k++] = q;
    }
    s_nparts = k;
    k = 0;
    for (int i = 0; i < s_nrings; ++i)
        if ((s_rings[i].age += 1.0f) < s_rings[i].life) s_rings[k++] = s_rings[i];
    s_nrings = k;
    k = 0;
    for (int i = 0; i < s_nlights; ++i)
        if ((s_lights[i].age += 1.0f) < s_lights[i].life) s_lights[k++] = s_lights[i];
    s_nlights = k;
}

void cyc_fx_particle(const CycFxParticle *p)
{
    if (s_nparts >= MAX_PARTS || p->life <= 0.0f || p->size <= 0.0f) return;
    s_parts[s_nparts++] = (Part){ *p, 0.0f };
}

void cyc_fx_burst(const CycFxParticle *base, int count, float speed)
{
    for (int i = 0; i < count; ++i) {
        CycFxParticle p = *base;
        float a = cyc_fx_random() * 6.2831853f, v = speed * (0.4f + 0.6f * cyc_fx_random());
        p.vx += cosf(a) * v;
        p.vy += sinf(a) * v;
        p.life *= 0.7f + 0.6f * cyc_fx_random();
        p.size *= 0.7f + 0.6f * cyc_fx_random();
        cyc_fx_particle(&p);
    }
}

void cyc_fx_ring(float x, float y, float radius, float life, float alpha, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_nrings >= MAX_RINGS || life <= 0.0f) return;
    s_rings[s_nrings++] = (Ring){ x, y, radius, life, alpha, 0.0f, r, g, b };
}

void cyc_fx_light(float x, float y, float radius, float life, float amount, float r, float g, float b)
{
    if (s_nlights >= MAX_LIGHTS || life <= 0.0f || radius <= 0.0f) return;
    s_lights[s_nlights++] = (Light){ x, y, radius, life, amount, r, g, b, 0.0f };
}

static void fire_colour(float t, float *r, float *g, float *b)
{
    if (t < 0.2f)      { *r = 255; *g = 248; *b = 200; }   /* white-yellow core */
    else if (t < 0.45f) { *r = 255; *g = 190; *b = 70; }    /* yellow-orange */
    else if (t < 0.75f) { *r = 240; *g = 110; *b = 30; }    /* orange-red */
    else               { *r = 150; *g = 50;  *b = 25; }    /* dying red */
}

static void blend(uint32_t *px, float cr, float cg, float cb, float a, int additive)
{
    uint32_t q = *px;
    float r = (float)((q >> 16) & 255), g = (float)((q >> 8) & 255), b = (float)(q & 255);
    if (additive) { r += cr * a; g += cg * a; b += cb * a; }
    else { r += (cr - r) * a; g += (cg - g) * a; b += (cb - b) * a; }
    *px = pack(r, g, b, q & 0xff000000u);
}

static void draw_lights(uint32_t *pic, int w, int h, float ox, float oy, int top)
{
    for (int i = 0; i < s_nlights; ++i) {
        const Light *l = &s_lights[i];
        float t = l->age / l->life, amount = l->amount * (1.0f - t) * (1.0f - t) * s_strength;
        float cx = l->x + ox, cy = l->y + oy, inv = 1.0f / l->radius;
        int ax = (int)(cx - l->radius), bx = (int)(cx + l->radius), ay = (int)(cy - l->radius), by = (int)(cy + l->radius);
        if (ax < 0) ax = 0; if (bx > w - 1) bx = w - 1; if (ay < top) ay = top; if (by > h - 1) by = h - 1;
        for (int y = ay; y <= by; ++y)
            for (int x = ax; x <= bx; ++x) {
                float dx = (float)x - cx, dy = (float)y - cy, d = sqrtf(dx * dx + dy * dy) * inv;
                if (d >= 1.0f) continue;
                float f = amount * (1.0f - d) * (1.0f - d);
                uint32_t q = pic[y * w + x];
                float r = (float)((q >> 16) & 255), g = (float)((q >> 8) & 255), b = (float)(q & 255);
                /* the surface catches the light in its own colour, plus a glow for dark surfaces */
                pic[y * w + x] = pack(r + (r + 60.0f) * f * l->r, g + (g + 60.0f) * f * l->g,
                                      b + (b + 60.0f) * f * l->b, q & 0xff000000u);
            }
    }
}

static void draw_rings(uint32_t *pic, int w, int h, float ox, float oy, int top)
{
    for (int i = 0; i < s_nrings; ++i) {
        const Ring *g = &s_rings[i];
        float t = g->age / g->life, e = 1.0f - (1.0f - t) * (1.0f - t);   /* ease out */
        float rad = g->radius * e, a = g->alpha * (1.0f - t) * s_strength, thick = 1.5f;
        float cx = g->x + ox, cy = g->y + oy;
        int ax = (int)(cx - rad - 2), bx = (int)(cx + rad + 2), ay = (int)(cy - rad - 2), by = (int)(cy + rad + 2);
        if (ax < 0) ax = 0; if (bx > w - 1) bx = w - 1; if (ay < top) ay = top; if (by > h - 1) by = h - 1;
        for (int y = ay; y <= by; ++y)
            for (int x = ax; x <= bx; ++x) {
                float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                float d = fabsf(sqrtf(dx * dx + dy * dy) - rad);
                if (d >= thick) continue;
                blend(&pic[y * w + x], g->r, g->g, g->b, a * (1.0f - d / thick), 1);
            }
    }
}

static void draw_disc(uint32_t *pic, int w, int h, int top, float sx, float sy, float radius,
                      float cr, float cg, float cb, float alpha, int additive)
{
    int ax = (int)(sx - radius - 1), bx = (int)(sx + radius + 1), ay = (int)(sy - radius - 1), by = (int)(sy + radius + 1);
    if (ax < 0) ax = 0; if (bx > w - 1) bx = w - 1; if (ay < top) ay = top; if (by > h - 1) by = h - 1;
    for (int y = ay; y <= by; ++y)
        for (int x = ax; x <= bx; ++x) {
            float dx = (float)x + 0.5f - sx, dy = (float)y + 0.5f - sy, d = sqrtf(dx * dx + dy * dy) / radius;
            if (d >= 1.0f) continue;
            blend(&pic[y * w + x], cr, cg, cb, alpha * (1.0f - d * d), additive);
        }
}

static void draw_parts(uint32_t *pic, int w, int h, float ox, float oy, int top)
{
    for (int i = 0; i < s_nparts; ++i) {
        const Part *q = &s_parts[i];
        const CycFxParticle *p = &q->p;
        float t = q->age / p->life, alpha = p->alpha * (1.0f - t) * s_strength;
        float cr = p->r, cg = p->g, cb = p->b;
        if (p->fire) fire_colour(t, &cr, &cg, &cb);
        float sx = p->x + ox, sy = p->y + oy;
        if (alpha > 1.0f) alpha = 1.0f;
        switch (p->look) {
        case CYC_FX_GLOW: draw_disc(pic, w, h, top, sx, sy, p->size, cr, cg, cb, alpha, 1); break;
        case CYC_FX_DUST: draw_disc(pic, w, h, top, sx, sy, p->size, cr, cg, cb, alpha * 0.6f, 0); break;
        case CYC_FX_SPARK: {                                    /* head plus a tail behind it */
            float speed = sqrtf(p->vx * p->vx + p->vy * p->vy);
            float ux = speed > 1e-3f ? -p->vx / speed : 0.0f, uy = speed > 1e-3f ? -p->vy / speed : 0.0f;
            int steps = (int)(speed * 2.5f) + 2;
            for (int s = 0; s < steps; ++s) {
                int x = (int)(sx + ux * (float)s), y = (int)(sy + uy * (float)s);
                if (x < 0 || x >= w || y < top || y >= h) continue;
                blend(&pic[y * w + x], cr, cg, cb, alpha * (1.0f - (float)s / (float)steps), 1);
            }
            break;
        }
        case CYC_FX_CHUNK: {
            int half = (int)(p->size + 0.5f), x0 = (int)sx - half / 2, y0 = (int)sy - half / 2;
            for (int y = y0; y < y0 + (half ? half : 1); ++y)
                for (int x = x0; x < x0 + (half ? half : 1); ++x)
                    if (x >= 0 && x < w && y >= top && y < h) blend(&pic[y * w + x], cr, cg, cb, alpha, 0);
            break;
        }
        }
    }
}

void cyc_fx_draw(uint32_t *pic, int w, int h, float origin_x, float origin_y, int clip_top)
{
    if (!pic || w <= 0 || h <= 0 || !cyc_fx_active() || s_strength <= 0.0f) return;
    if (clip_top < 0) clip_top = 0;
    draw_lights(pic, w, h, origin_x, origin_y, clip_top);
    draw_rings(pic, w, h, origin_x, origin_y, clip_top);
    draw_parts(pic, w, h, origin_x, origin_y, clip_top);
}
