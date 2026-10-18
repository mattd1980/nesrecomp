/* cyc_touch.c - on-screen touch controls (cyc_touch.h). */
#include "cyc_touch.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FINGERS 10
#define PI 3.14159265f

typedef struct { float x, y, r; } Circle;
typedef struct { float x, y, w, h; } Box;
typedef struct {
    int w, h;
    Circle dpad, a, b, ab;
    Box select, start, menu;
} Layout;

typedef struct { SDL_FingerID id; float x, y; bool down; } Finger;

static bool s_enabled;
static Finger s_fingers[MAX_FINGERS];
static Layout s_lay;
static uint8_t s_buttons;
static bool s_menu;
/* A tap can begin and end between two game frames, so each press is reported
 * for at least MIN_PRESS_MS (bits 0-7 = buttons, 8 = menu). */
#define MIN_PRESS_MS 70
static Uint64 s_press_until[9];
static SDL_Window *s_window;
static SDL_SpinLock s_lock;            /* touches arrive on Android's UI thread */

static void handle(const SDL_Event *ev, int window_w, int window_h);

/* Watch fingers before SDL's renderer does: with a logical size set, its own
 * watcher rewrites finger coordinates relative to the picture and clamps them,
 * which would fold every touch below a portrait picture onto its edge. */
static int SDLCALL watch(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERMOTION && ev->type != SDL_FINGERUP) return 1;
    SDL_Window *win = s_window ? s_window : SDL_GetWindowFromID(ev->tfinger.windowID);
    int w = 0, h = 0;
    if (win) SDL_GetWindowSizeInPixels(win, &w, &h);
    if (w > 0 && h > 0) handle(ev, w, h);
    return 1;
}

void cyc_touch_init(void)
{
#ifdef __ANDROID__
    s_enabled = true;
#else
    s_enabled = false;
#endif
    const char *env = getenv("NESRECOMP_TOUCH");
    if (env && *env) s_enabled = strcmp(env, "0") != 0;
    /* a desktop mouse plays the finger when the controls are forced on */
    if (s_enabled) SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    memset(s_fingers, 0, sizeof(s_fingers));
    if (s_enabled) SDL_AddEventWatch(watch, NULL);   /* before the renderer adds its own */
}

void cyc_touch_set_window(SDL_Window *window) { s_window = window; }

bool cyc_touch_enabled(void) { return s_enabled; }

static void layout(int w, int h)
{
    if (w == s_lay.w && h == s_lay.h) return;
    s_lay.w = w; s_lay.h = h;
    float m = (float)(w < h ? w : h);
    bool portrait = h > w;
    float r = m * (portrait ? 0.20f : 0.22f);          /* D-pad radius */
    float by = portrait ? (float)h - r * 1.35f : (float)h - r * 1.2f;
    s_lay.dpad = (Circle){ r * 1.2f, by, r };
    float br = r * 0.6f;                               /* A/B radius */
    s_lay.a = (Circle){ (float)w - br * 1.4f, by - br * 0.55f, br };
    s_lay.b = (Circle){ (float)w - br * (portrait ? 3.4f : 3.8f), by + br * 0.55f, br };
    s_lay.ab = (Circle){ (s_lay.a.x + s_lay.b.x) * 0.5f, (s_lay.a.y + s_lay.b.y) * 0.5f, br * 0.4f };
    float pw = m * 0.16f, ph = m * 0.07f, py = (float)h - ph * 1.4f;
    s_lay.select = (Box){ (float)w * 0.5f - pw * 1.15f, py, pw, ph };
    s_lay.start = (Box){ (float)w * 0.5f + pw * 0.15f, py, pw, ph };
    float mb = m * 0.105f;
    s_lay.menu = (Box){ (float)w - mb * 1.3f, mb * 0.3f, mb, mb };
}

static bool in_circle(const Circle *c, float x, float y, float slack)
{
    float dx = x - c->x, dy = y - c->y;
    return dx * dx + dy * dy <= c->r * c->r * slack * slack;
}

static bool in_box(const Box *b, float x, float y, float slack)
{
    float sx = b->w * (slack - 1.0f) * 0.5f, sy = b->h * (slack - 1.0f) * 0.5f;
    return x >= b->x - sx && x <= b->x + b->w + sx && y >= b->y - sy && y <= b->y + b->h + sy;
}

/* recompute the held buttons from every finger down */
static void evaluate(void)
{
    uint8_t bits = 0;
    bool menu = false;
    for (int i = 0; i < MAX_FINGERS; ++i) {
        const Finger *f = &s_fingers[i];
        if (!f->down) continue;
        float x = f->x, y = f->y;
        if (in_circle(&s_lay.dpad, x, y, 1.35f)) {
            float dx = x - s_lay.dpad.x, dy = y - s_lay.dpad.y;
            if (dx * dx + dy * dy < s_lay.dpad.r * s_lay.dpad.r * 0.04f) continue;   /* dead centre */
            float ang = atan2f(dy, dx);                     /* 0 = right, +pi/2 = down */
            int sector = (int)floorf((ang + PI) / (PI / 4.0f) + 0.5f) & 7;
            static const uint8_t DIR[8] = {   /* from left, clockwise on screen (y grows down) */
                0x02, 0x0A, 0x08, 0x09, 0x01, 0x05, 0x04, 0x06 };
            bits |= DIR[sector];
        } else if (in_circle(&s_lay.ab, x, y, 1.0f)) bits |= 0x80 | 0x40;
        else if (in_circle(&s_lay.a, x, y, 1.3f)) bits |= 0x80;
        else if (in_circle(&s_lay.b, x, y, 1.3f)) bits |= 0x40;
        else if (in_box(&s_lay.start, x, y, 1.4f)) bits |= 0x10;
        else if (in_box(&s_lay.select, x, y, 1.4f)) bits |= 0x20;
        else if (in_box(&s_lay.menu, x, y, 1.4f)) menu = true;
    }
    if ((bits & 0x0C) == 0x0C) bits &= (uint8_t)~0x0C;
    if ((bits & 0x03) == 0x03) bits &= (uint8_t)~0x03;
    Uint64 until = SDL_GetTicks64() + MIN_PRESS_MS;
    for (int i = 0; i < 8; ++i)
        if ((bits & ~s_buttons) & (1u << i)) s_press_until[i] = until;
    if (menu && !s_menu) s_press_until[8] = until;
    s_buttons = bits;
    s_menu = menu;
}

static void handle(const SDL_Event *ev, int window_w, int window_h)
{
    SDL_AtomicLock(&s_lock);
    layout(window_w, window_h);
    const SDL_TouchFingerEvent *t = &ev->tfinger;
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (s_fingers[i].down && s_fingers[i].id == t->fingerId) slot = i;
        if (!s_fingers[i].down && free_slot < 0) free_slot = i;
    }
    if (ev->type == SDL_FINGERDOWN && slot < 0) slot = free_slot;
    if (slot < 0) { SDL_AtomicUnlock(&s_lock); return; }
    Finger *f = &s_fingers[slot];
    if (ev->type == SDL_FINGERUP) f->down = false;
    else { f->id = t->fingerId; f->down = true; f->x = t->x * (float)window_w; f->y = t->y * (float)window_h; }
    evaluate();
    if (getenv("NESRECOMP_TOUCH_TRACE"))
        printf("[touch] type %x finger %lld at %.0f,%.0f of %dx%d -> buttons %02x menu %d\n", ev->type,
               (long long)t->fingerId, f->x, f->y, window_w, window_h, s_buttons, s_menu), fflush(stdout);
    SDL_AtomicUnlock(&s_lock);
}

uint8_t cyc_touch_buttons(void)
{
    if (!s_enabled) return 0;
    Uint64 now = SDL_GetTicks64();
    SDL_AtomicLock(&s_lock);
    uint8_t b = s_buttons;
    for (int i = 0; i < 8; ++i)
        if (s_press_until[i] > now) b |= (uint8_t)(1u << i);
    SDL_AtomicUnlock(&s_lock);
    return b;
}

bool cyc_touch_menu_held(void)
{
    if (!s_enabled) return false;
    Uint64 now = SDL_GetTicks64();
    SDL_AtomicLock(&s_lock);
    bool m = s_menu || s_press_until[8] > now;
    SDL_AtomicUnlock(&s_lock);
    return m;
}

void cyc_touch_release_all(void)
{
    SDL_AtomicLock(&s_lock);
    memset(s_fingers, 0, sizeof(s_fingers));
    memset(s_press_until, 0, sizeof(s_press_until));
    s_buttons = 0;
    s_menu = false;
    SDL_AtomicUnlock(&s_lock);
}

/* ---- drawing ---- */

static void disc(SDL_Renderer *ren, float cx, float cy, float r, SDL_Color c)
{
    enum { SEG = 40 };
    SDL_Vertex v[SEG + 2];
    int idx[SEG * 3];
    v[0] = (SDL_Vertex){ { cx, cy }, c, { 0, 0 } };
    for (int i = 0; i <= SEG; ++i) {
        float a = (float)i * 2.0f * PI / (float)SEG;
        v[i + 1] = (SDL_Vertex){ { cx + cosf(a) * r, cy + sinf(a) * r }, c, { 0, 0 } };
    }
    for (int i = 0; i < SEG; ++i) { idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = i + 2; }
    SDL_RenderGeometry(ren, NULL, v, SEG + 2, idx, SEG * 3);
}

static void ring(SDL_Renderer *ren, float cx, float cy, float r, float t, SDL_Color c)
{
    enum { SEG = 48 };
    SDL_Vertex v[SEG * 2 + 2];
    int idx[SEG * 6];
    for (int i = 0; i <= SEG; ++i) {
        float a = (float)i * 2.0f * PI / (float)SEG, ca = cosf(a), sa = sinf(a);
        v[i * 2] = (SDL_Vertex){ { cx + ca * r, cy + sa * r }, c, { 0, 0 } };
        v[i * 2 + 1] = (SDL_Vertex){ { cx + ca * (r - t), cy + sa * (r - t) }, c, { 0, 0 } };
    }
    for (int i = 0; i < SEG; ++i) {
        int k = i * 2;
        idx[i * 6] = k; idx[i * 6 + 1] = k + 1; idx[i * 6 + 2] = k + 2;
        idx[i * 6 + 3] = k + 1; idx[i * 6 + 4] = k + 3; idx[i * 6 + 5] = k + 2;
    }
    SDL_RenderGeometry(ren, NULL, v, SEG * 2 + 2, idx, SEG * 6);
}

static void rect(SDL_Renderer *ren, const Box *b, SDL_Color c)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_FRect r = { b->x, b->y, b->w, b->h };
    SDL_RenderFillRectF(ren, &r);
}

/* a filled triangle pointing along (dx, dy) */
static void arrow(SDL_Renderer *ren, float cx, float cy, float dx, float dy, float s, SDL_Color c)
{
    float px = -dy, py = dx;
    SDL_Vertex v[3] = {
        { { cx + dx * s, cy + dy * s }, c, { 0, 0 } },
        { { cx - dx * s * 0.5f + px * s * 0.8f, cy - dy * s * 0.5f + py * s * 0.8f }, c, { 0, 0 } },
        { { cx - dx * s * 0.5f - px * s * 0.8f, cy - dy * s * 0.5f - py * s * 0.8f }, c, { 0, 0 } },
    };
    SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}

void cyc_touch_draw(SDL_Renderer *ren)
{
    if (!s_enabled) return;
    int lw = 0, lh = 0, ow = 0, oh = 0;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_bool integer = SDL_RenderGetIntegerScale(ren);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_RenderSetLogicalSize(ren, 0, 0);                  /* window pixels */
    SDL_RenderSetViewport(ren, NULL);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    SDL_AtomicLock(&s_lock);
    layout(ow, oh);
    Layout lay = s_lay;
    uint8_t b = s_buttons;
    bool menu = s_menu;
    SDL_AtomicUnlock(&s_lock);
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(ren, &old);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    const SDL_Color base = { 255, 255, 255, 46 }, edge = { 255, 255, 255, 110 }, lit = { 255, 210, 90, 150 };
    const SDL_Color glyph = { 255, 255, 255, 150 }, glyph_lit = { 40, 30, 10, 220 };
    /* D-pad: a disc, a ring and four arrows, the held ones lit */
    disc(ren, lay.dpad.x, lay.dpad.y, lay.dpad.r, base);
    ring(ren, lay.dpad.x, lay.dpad.y, lay.dpad.r, lay.dpad.r * 0.05f, edge);
    float as = lay.dpad.r * 0.18f, ad = lay.dpad.r * 0.66f;
    const struct { float dx, dy; uint8_t bit; } ARROWS[4] = { { 0, -1, 0x08 }, { 0, 1, 0x04 }, { -1, 0, 0x02 }, { 1, 0, 0x01 } };
    for (int i = 0; i < 4; ++i) {
        float x = lay.dpad.x + ARROWS[i].dx * ad, y = lay.dpad.y + ARROWS[i].dy * ad;
        if (b & ARROWS[i].bit) disc(ren, x, y, as * 1.9f, lit);
        arrow(ren, x, y, ARROWS[i].dx, ARROWS[i].dy, as, (b & ARROWS[i].bit) ? glyph_lit : glyph);
    }
    /* B and A */
    const Circle *btn[2] = { &lay.b, &lay.a };
    const uint8_t bit[2] = { 0x40, 0x80 };
    for (int i = 0; i < 2; ++i) {
        disc(ren, btn[i]->x, btn[i]->y, btn[i]->r, (b & bit[i]) ? lit : base);
        ring(ren, btn[i]->x, btn[i]->y, btn[i]->r, btn[i]->r * 0.09f, edge);
        /* the letter, as two strokes: B = a bar and two bumps is too fussy; a dot marks B, a bar marks A */
        if (i == 0) disc(ren, btn[i]->x, btn[i]->y, btn[i]->r * 0.22f, (b & bit[i]) ? glyph_lit : glyph);
        else {
            Box bar = { btn[i]->x - btn[i]->r * 0.08f, btn[i]->y - btn[i]->r * 0.38f, btn[i]->r * 0.16f, btn[i]->r * 0.76f };
            rect(ren, &bar, (b & bit[i]) ? glyph_lit : glyph);
        }
    }
    /* Select, Start, Menu */
    rect(ren, &lay.select, (b & 0x20) ? lit : base);
    rect(ren, &lay.start, (b & 0x10) ? lit : base);
    rect(ren, &lay.menu, menu ? lit : base);
    float lx = lay.menu.x + lay.menu.w * 0.22f, lwid = lay.menu.w * 0.56f, lh2 = lay.menu.h * 0.09f;
    for (int i = 0; i < 3; ++i) {                           /* the three-line menu glyph */
        Box line = { lx, lay.menu.y + lay.menu.h * (0.27f + 0.2f * (float)i), lwid, lh2 };
        rect(ren, &line, glyph);
    }

    SDL_SetRenderDrawBlendMode(ren, old);
    SDL_RenderSetLogicalSize(ren, lw, lh);                  /* back to the picture's coordinates */
    SDL_RenderSetIntegerScale(ren, integer);
}
