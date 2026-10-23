/*
 * cyc_chat.c - chat over the game and chat effects on it (cyc_chat.h).
 */
#include "cyc_chat.h"

#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_state.h"
#include "cyc_tcp.h"

#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyc_chat_font.inc"

#define MSG_MAX   48
#define PAD_MAX   8
#define RAM_MAX   16
#define ROWS_MAX  16
#define LINE_H    10                 /* chat pixels per panel row: 8 of glyph, 2 of gap */
#define FADE_MS   1000

typedef struct {
    char     user[32];
    char     text[256];
    uint32_t color;
    uint32_t born, life;
} Msg;

enum { PAD_PRESS, PAD_TAP, PAD_BLOCK, PAD_INVERT, PAD_LAG, PAD_DRUNK };
typedef struct {
    bool    on;
    int     mode;
    uint8_t buttons;
    int     delay;                   /* lag: frames; drunk: frames between lurches */
    long    left;
    char    label[40];
} PadFx;
#define LAG_MAX 120
static uint8_t s_lag[LAG_MAX];       /* player 1's buttons, the last LAG_MAX frames */
static unsigned s_lag_at;
static unsigned s_drunk_seed;        /* a new lurch pattern per drunk effect */

enum { RAM_SET, RAM_ADD, RAM_OR, RAM_AND };
/* How a number is stored: binary little- or big-endian, packed BCD (most
 * significant byte first), or one decimal digit per byte (most significant
 * first, as status bars often keep them). */
enum { ENC_BIN, ENC_BIN_BE, ENC_BCD, ENC_DIGITS };
typedef struct {
    bool     on;
    uint16_t addr;
    int      op, enc, len;
    long     value, lo, hi;
    long     left, every, tick;
    long     if_addr, if_eq;
    char     label[40];
} RamFx;

static Msg s_msg[MSG_MAX];
static int s_msg_next, s_msg_count;  /* ring; the newest is at s_msg_next - 1 */
static struct { char text[96]; uint32_t color, born, life; } s_banner;
#define POLL_MAX 5
static struct {
    char     title[32], option[POLL_MAX][24];
    int      votes[POLL_MAX], count, winner, y;
    bool     countdown;
    uint32_t born, life;
} s_poll;
static PadFx s_pad[PAD_MAX];
static RamFx s_ram[RAM_MAX];
/* offset: percent of the window height between the panel and its edge */
static struct { bool show, right, top; int lines, scale, offset; uint32_t ms; } s_cfg = { true, false, false, 8, 0, 3, 20000 };

static SDL_Texture *s_tex;
static int          s_tex_w, s_tex_h;
static uint32_t    *s_px;
static size_t       s_px_len;

/* ---- effects ---- */

static uint8_t swap_bits(uint8_t pad, uint8_t a, uint8_t b)
{
    bool ha = pad & a, hb = pad & b;
    pad &= (uint8_t)~(a | b);
    return (uint8_t)(pad | (ha ? b : 0) | (hb ? a : 0));
}

static long ram_decode(uint16_t addr, int enc, int len)
{
    long v = 0;
    for (int i = 0; i < len; ++i) {
        uint8_t b = cyc_mod_peek((uint16_t)(addr + i));
        switch (enc) {
        case ENC_BIN: v |= (long)b << (8 * i); break;
        case ENC_BIN_BE: v = v << 8 | b; break;
        case ENC_BCD: v = v * 100 + (b >> 4) * 10 + (b & 15); break;
        default: v = v * 10 + b % 10; break;
        }
    }
    return v;
}

static void ram_encode(uint16_t addr, int enc, int len, long v)
{
    for (int i = len - 1; i >= 0; --i) {
        uint8_t b;
        int at = i;
        switch (enc) {
        case ENC_BIN: b = (uint8_t)(v >> (8 * i)); break;
        case ENC_BIN_BE: b = (uint8_t)v; v >>= 8; break;
        case ENC_BCD: b = (uint8_t)((v / 10 % 10) << 4 | v % 10); v /= 100; break;
        default: b = (uint8_t)(v % 10); v /= 10; break;
        }
        cyc_mod_poke((uint16_t)(addr + at), b);
    }
}

static void ram_store(RamFx *fx)
{
    uint8_t cond;
    if (fx->if_addr >= 0 && (!cyc_mod_peek_ok((uint16_t)fx->if_addr, &cond) || cond != (uint8_t)fx->if_eq)) return;
    if (fx->op == RAM_OR || fx->op == RAM_AND) {    /* bits of one byte */
        uint8_t v = cyc_mod_peek(fx->addr);
        cyc_mod_poke(fx->addr, (uint8_t)(fx->op == RAM_OR ? v | fx->value : v & fx->value));
        return;
    }
    long v = fx->op == RAM_SET ? fx->value : ram_decode(fx->addr, fx->enc, fx->len) + fx->value;
    if (v < fx->lo) v = fx->lo;
    if (v > fx->hi) v = fx->hi;
    ram_encode(fx->addr, fx->enc, fx->len, v);
}

static void rewind_record(void);

uint8_t cyc_chat_frame(uint8_t pad)
{
    rewind_record();
    for (int i = 0; i < RAM_MAX; ++i) {
        RamFx *fx = &s_ram[i];
        if (!fx->on) continue;
        if (fx->tick++ % fx->every == 0) ram_store(fx);
        if (--fx->left <= 0) fx->on = false;
    }
    s_lag[s_lag_at++ % LAG_MAX] = pad;     /* what the player pressed, before any effect */
    for (int i = 0; i < PAD_MAX; ++i) {
        PadFx *fx = &s_pad[i];
        if (!fx->on) continue;
        switch (fx->mode) {
        case PAD_PRESS: pad |= fx->buttons; break;
        case PAD_TAP: if ((fx->left / 8) % 2 == 0) pad |= fx->buttons; break;
        case PAD_BLOCK: pad &= (uint8_t)~fx->buttons; break;
        case PAD_LAG: {                       /* the buttons named come in `delay` frames late */
            uint8_t late = s_lag[(s_lag_at - 1 - (unsigned)fx->delay) % LAG_MAX];
            pad = (uint8_t)((pad & ~fx->buttons) | (late & fx->buttons));
            break;
        }
        case PAD_DRUNK:                       /* one of the buttons named, now and then, held a while */
            if ((fx->left / fx->delay) % 3 == 0) {
                uint8_t bit[8];
                int n = 0;
                for (int b = 0; b < 8; ++b)
                    if (fx->buttons >> b & 1) bit[n++] = (uint8_t)(1 << b);
                unsigned spin = (unsigned)(fx->left / fx->delay) * 2654435761u + s_drunk_seed;
                pad |= bit[(spin >> 16) % (unsigned)n];
            }
            break;
        default:                              /* invert / swap: each pair named trades places */
            if (fx->buttons & 0x03) pad = swap_bits(pad, 0x01, 0x02);
            if (fx->buttons & 0x0C) pad = swap_bits(pad, 0x04, 0x08);
            if ((fx->buttons & 0xC0) == 0xC0) pad = swap_bits(pad, 0x40, 0x80);
            break;
        }
        if (--fx->left <= 0) fx->on = false;
    }
    return pad;
}

/* The slot an effect goes in: the active one with its label, else a free one. */
static int pad_slot(const char *label)
{
    for (int i = 0; label[0] && i < PAD_MAX; ++i)
        if (s_pad[i].on && !strcmp(s_pad[i].label, label)) return i;
    for (int i = 0; i < PAD_MAX; ++i)
        if (!s_pad[i].on) return i;
    return -1;
}

static int ram_slot(const char *label)
{
    for (int i = 0; label[0] && i < RAM_MAX; ++i)
        if (s_ram[i].on && !strcmp(s_ram[i].label, label)) return i;
    for (int i = 0; i < RAM_MAX; ++i)
        if (!s_ram[i].on) return i;
    return -1;
}

/* ---- the picture, the clock and the sound: effects any game takes ---- */

enum { FX_FLIP_H, FX_FLIP_V, FX_GRAY, FX_NEGATIVE, FX_RAINBOW, FX_TINT, FX_PIXELATE, FX_SHAKE, FX_ZOOM,
       FX_WOBBLE, FX_SPOTLIGHT, FX_MIRROR, FX_KINDS };
static const char *const FX_NAMES[FX_KINDS] = { "flip_h", "flip_v", "gray", "negative", "rainbow", "tint",
    "pixelate", "shake", "zoom", "wobble", "spotlight", "mirror" };
typedef struct {
    bool     on;
    long     amount;
    uint32_t color, born, life;
    int      x, y;                   /* spotlight centre, percent of the picture */
    char     label[40];
} PicFx;
static PicFx s_fx[FX_KINDS];         /* one of each kind; a new one replaces it */

static struct { bool on; int percent; uint32_t born, life; char label[40]; } s_speed, s_stop;
static struct { bool on; uint8_t mask; uint32_t born, life; char label[40]; } s_mute;

#define REWIND_MAX   24              /* one snapshot every REWIND_EVERY frames */
#define REWIND_EVERY 30
static struct { uint8_t *data; size_t len; long frame; } s_rewind[REWIND_MAX], s_checkpoint;
static int  s_rewind_count;
static long s_rewind_frames;         /* frames run since rewind was switched on */
static bool s_rewind_on;
static CycChatHost s_host;

void cyc_chat_set_host(const CycChatHost *host) { s_host = *host; }

static bool live(uint32_t now, uint32_t born, uint32_t life) { return life && now - born < life; }

static bool online(void) { return s_host.online && s_host.online(); }

static void snap_free(uint8_t **data)
{
    free(*data);
    *data = NULL;
}

static void rewind_record(void)
{
    if (!s_rewind_on || s_rewind_frames++ % REWIND_EVERY) return;
    uint8_t *data;
    size_t len;
    char err[128];
    if (!cyc_state_save(&data, &len, err, sizeof(err))) return;
    if (s_rewind_count == REWIND_MAX) {
        snap_free(&s_rewind[0].data);
        memmove(s_rewind, s_rewind + 1, sizeof(s_rewind[0]) * (REWIND_MAX - 1));
        s_rewind_count--;
    }
    s_rewind[s_rewind_count].data = data;
    s_rewind[s_rewind_count].len = len;
    s_rewind[s_rewind_count].frame = s_rewind_frames;
    s_rewind_count++;
}

static void rewind_clear(void)
{
    for (int i = 0; i < s_rewind_count; ++i) snap_free(&s_rewind[i].data);
    s_rewind_count = 0;
}

static bool state_apply(const uint8_t *data, size_t len, char *err, size_t n)
{
    if (!cyc_state_load(data, len, err, n)) return false;
    if (s_host.state_loaded) s_host.state_loaded();
    return true;
}

float cyc_chat_pace(void)
{
    uint32_t now = SDL_GetTicks();
    if (s_speed.on && !live(now, s_speed.born, s_speed.life)) s_speed.on = false;
    return s_speed.on ? 100.0f / (float)s_speed.percent : 1.0f;
}

bool cyc_chat_frozen(void)
{
    if (s_stop.on && !live(SDL_GetTicks(), s_stop.born, s_stop.life)) s_stop.on = false;
    return s_stop.on;
}

uint8_t cyc_chat_muted(void)
{
    if (s_mute.on && !live(SDL_GetTicks(), s_mute.born, s_mute.life)) s_mute.on = false;
    return s_mute.on ? s_mute.mask : 0;
}

static uint32_t hue_rotate(uint32_t c, float cs, float sn)
{
    /* rotate the chroma plane of YIQ */
    float r = (float)(c >> 16 & 255), g = (float)(c >> 8 & 255), b = (float)(c & 255);
    float y = 0.299f * r + 0.587f * g + 0.114f * b, i = 0.596f * r - 0.274f * g - 0.322f * b,
          q = 0.211f * r - 0.523f * g + 0.312f * b;
    float i2 = i * cs - q * sn, q2 = i * sn + q * cs;
    float rr = y + 0.956f * i2 + 0.621f * q2, gg = y - 0.272f * i2 - 0.647f * q2, bb = y - 1.106f * i2 + 1.703f * q2;
#define CLAMP8(v) ((uint32_t)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))
    return 0xFF000000u | CLAMP8(rr) << 16 | CLAMP8(gg) << 8 | CLAMP8(bb);
#undef CLAMP8
}

const uint32_t *cyc_chat_picture(const uint32_t *pic, int w, int h)
{
    static uint32_t *out;
    static size_t out_len;
    uint32_t now = SDL_GetTicks();
    bool any = false;
    for (int k = 0; k < FX_KINDS; ++k) {
        if (s_fx[k].on && !live(now, s_fx[k].born, s_fx[k].life)) s_fx[k].on = false;
        any |= s_fx[k].on;
    }
    if (!any || w <= 0 || h <= 0) return pic;
    size_t need = (size_t)w * (size_t)h;
    if (need > out_len) {
        free(out);
        out = (uint32_t *)malloc(need * 4);
        out_len = out ? need : 0;
        if (!out) return pic;
    }
    const PicFx *f = s_fx;
    float t = (float)now / 1000.0f;
    int block = f[FX_PIXELATE].on ? (int)(f[FX_PIXELATE].amount < 2 ? 2 : f[FX_PIXELATE].amount > 32 ? 32 : f[FX_PIXELATE].amount) : 1;
    int dx = 0, dy = 0;
    if (f[FX_SHAKE].on) {
        int a = (int)(f[FX_SHAKE].amount < 1 ? 1 : f[FX_SHAKE].amount > 32 ? 32 : f[FX_SHAKE].amount);
        dx = rand() % (2 * a + 1) - a;
        dy = rand() % (2 * a + 1) - a;
    }
    float zoom = f[FX_ZOOM].on ? (float)(f[FX_ZOOM].amount < 100 ? 100 : f[FX_ZOOM].amount > 400 ? 400 : f[FX_ZOOM].amount) / 100.0f : 1.0f;
    float wob = f[FX_WOBBLE].on ? (float)(f[FX_WOBBLE].amount < 1 ? 1 : f[FX_WOBBLE].amount > 32 ? 32 : f[FX_WOBBLE].amount) : 0.0f;
    /* zoom closes in on x,y (the player, when a bridge follows one) */
    float cx = (float)w * (float)(f[FX_ZOOM].on ? f[FX_ZOOM].x : 50) / 100.0f, cy = (float)h * (float)(f[FX_ZOOM].on ? f[FX_ZOOM].y : 50) / 100.0f;
    for (int y = 0; y < h; ++y) {
        float row_shift = wob ? wob * sinf(6.2831853f * ((float)y / 48.0f + t * 0.8f)) : 0.0f;
        for (int x = 0; x < w; ++x) {
            int bx = x - x % block, by = y - y % block;
            float sx = (float)(bx - dx) + row_shift, sy = (float)(by - dy);
            if (zoom != 1.0f) { sx = cx + (sx - cx) / zoom; sy = cy + (sy - cy) / zoom; }
            int ix = (int)sx, iy = (int)sy;
            if (f[FX_MIRROR].on && ix >= w / 2) ix = w - 1 - ix;
            if (f[FX_FLIP_H].on) ix = w - 1 - ix;
            if (f[FX_FLIP_V].on) iy = h - 1 - iy;
            out[(size_t)y * (size_t)w + (size_t)x] =
                ix < 0 || iy < 0 || ix >= w || iy >= h ? 0xFF000000u : pic[(size_t)iy * (size_t)w + (size_t)ix];
        }
    }
    float cs = 1, sn = 0;
    if (f[FX_RAINBOW].on) { cs = cosf(t * 3.0f); sn = sinf(t * 3.0f); }
    uint32_t tint = f[FX_TINT].on ? f[FX_TINT].color : 0;
    float spot_x = (float)w * (float)f[FX_SPOTLIGHT].x / 100.0f, spot_y = (float)h * (float)f[FX_SPOTLIGHT].y / 100.0f,
          spot_r = (float)h * (float)(f[FX_SPOTLIGHT].amount < 5 ? 5 : f[FX_SPOTLIGHT].amount > 100 ? 100 : f[FX_SPOTLIGHT].amount) / 100.0f;
    bool colour = f[FX_GRAY].on || f[FX_NEGATIVE].on || f[FX_RAINBOW].on || f[FX_TINT].on || f[FX_SPOTLIGHT].on;
    for (int y = 0; colour && y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t *p = &out[(size_t)y * (size_t)w + (size_t)x], c = *p;
            unsigned r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
            if (f[FX_GRAY].on) r = g = b = (r * 77 + g * 150 + b * 29) >> 8;
            if (f[FX_NEGATIVE].on) { r = 255 - r; g = 255 - g; b = 255 - b; }
            c = 0xFF000000u | r << 16 | g << 8 | b;
            if (f[FX_RAINBOW].on) c = hue_rotate(c, cs, sn);
            if (tint) {            /* half-way to the colour, keeping the brightness */
                unsigned l = ((c >> 16 & 255) * 77 + (c >> 8 & 255) * 150 + (c & 255) * 29) >> 8;
                r = ((c >> 16 & 255) + (tint >> 16 & 255) * l / 255) / 2;
                g = ((c >> 8 & 255) + (tint >> 8 & 255) * l / 255) / 2;
                b = ((c & 255) + (tint & 255) * l / 255) / 2;
                c = 0xFF000000u | r << 16 | g << 8 | b;
            }
            if (f[FX_SPOTLIGHT].on) {
                float ddx = (float)x - spot_x, ddy = (float)y - spot_y, d = sqrtf(ddx * ddx + ddy * ddy);
                float k = d <= spot_r ? 1.0f : d >= spot_r * 1.35f ? 0.06f : 1.0f - 0.94f * (d - spot_r) / (spot_r * 0.35f);
                c = 0xFF000000u | (uint32_t)((float)(c >> 16 & 255) * k) << 16 | (uint32_t)((float)(c >> 8 & 255) * k) << 8 |
                    (uint32_t)((float)(c & 255) * k);
            }
            *p = c;
        }
    return out;
}

/* ---- requests ---- */

static uint32_t color_field(const char *line, const char *key, uint32_t fallback)
{
    char hex[16];
    if (!cyc_tcp_str(line, key, hex, sizeof(hex))) return fallback;
    const char *p = hex[0] == '#' ? hex + 1 : hex;
    char *end;
    unsigned long v = strtoul(p, &end, 16);
    return end != p && !*end && v <= 0xFFFFFFu ? (uint32_t)v : fallback;
}

/* Printable ASCII only: the font has nothing else (the bridge transliterates). */
static void printable(char *s)
{
    for (; *s; ++s)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) *s = '?';
}

static void tcp_chat(int id, const char *line)
{
    Msg m;
    memset(&m, 0, sizeof(m));
    if (!cyc_tcp_str(line, "text", m.text, sizeof(m.text))) {
        cyc_tcp_err(id, "chat: user, text, color (RRGGBB), ms");
        return;
    }
    cyc_tcp_str(line, "user", m.user, sizeof(m.user));
    printable(m.user);
    printable(m.text);
    m.color = color_field(line, "color", 0x9FA8FF);
    long ms = (long)s_cfg.ms;
    cyc_tcp_long(line, "ms", &ms);
    m.born = SDL_GetTicks();
    m.life = (uint32_t)(ms < 500 ? 500 : ms > 600000 ? 600000 : ms);
    s_msg[s_msg_next] = m;
    s_msg_next = (s_msg_next + 1) % MSG_MAX;
    if (s_msg_count < MSG_MAX) s_msg_count++;
    cyc_tcp_ok(id, NULL);
}

static void tcp_banner(int id, const char *line)
{
    if (!cyc_tcp_str(line, "text", s_banner.text, sizeof(s_banner.text))) {
        cyc_tcp_err(id, "chat_banner: text, color (RRGGBB), ms");
        return;
    }
    printable(s_banner.text);
    s_banner.color = color_field(line, "color", 0xFFD25A);
    long ms = 3000;
    cyc_tcp_long(line, "ms", &ms);
    s_banner.born = SDL_GetTicks();
    s_banner.life = (uint32_t)(ms < 500 ? 500 : ms > 60000 ? 60000 : ms);
    cyc_tcp_ok(id, NULL);
}

/* `options` and `votes` are '|'-separated lists ("STAR|MOON", "3|1"); a
 * request without options takes the poll down. */
static void tcp_poll(int id, const char *line)
{
    char options[POLL_MAX * 24 + 8] = "", votes[POLL_MAX * 8 + 8] = "";
    cyc_tcp_str(line, "options", options, sizeof(options));
    cyc_tcp_str(line, "votes", votes, sizeof(votes));
    s_poll.count = 0;
    if (!options[0]) { cyc_tcp_ok(id, NULL); return; }
    char *o = options, *v = votes;
    while (o && s_poll.count < POLL_MAX) {
        char *bar = strchr(o, '|');
        if (bar) *bar = 0;
        snprintf(s_poll.option[s_poll.count], sizeof(s_poll.option[0]), "%s", o);
        printable(s_poll.option[s_poll.count]);
        long n = v && *v ? strtol(v, &v, 10) : 0;
        if (v && *v == '|') ++v;
        s_poll.votes[s_poll.count++] = n < 0 ? 0 : n > 99999 ? 99999 : (int)n;
        o = bar ? bar + 1 : NULL;
    }
    if (!cyc_tcp_str(line, "title", s_poll.title, sizeof(s_poll.title))) snprintf(s_poll.title, sizeof(s_poll.title), "VOTE");
    printable(s_poll.title);
    long ms = 10000, winner = -1, y = 20;
    bool countdown = true;
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_long(line, "winner", &winner);
    cyc_tcp_long(line, "y", &y);
    cyc_tcp_bool(line, "countdown", &countdown);
    s_poll.winner = winner >= 0 && winner < s_poll.count ? (int)winner : -1;
    s_poll.y = y < 0 ? 0 : y > 90 ? 90 : (int)y;
    s_poll.countdown = countdown;
    s_poll.born = SDL_GetTicks();
    s_poll.life = (uint32_t)(ms < 500 ? 500 : ms > 600000 ? 600000 : ms);
    cyc_tcp_ok(id, NULL);
}

static void tcp_pad(int id, const char *line)
{
    char mode[16] = "press", label[40] = "";
    long buttons = 0, frames = 60, delay = -1;
    cyc_tcp_str(line, "mode", mode, sizeof(mode));
    cyc_tcp_str(line, "label", label, sizeof(label));
    printable(label);
    cyc_tcp_long(line, "frames", &frames);
    cyc_tcp_long(line, "delay", &delay);
    int m = !strcmp(mode, "press") ? PAD_PRESS : !strcmp(mode, "tap") ? PAD_TAP
          : !strcmp(mode, "block") ? PAD_BLOCK : !strcmp(mode, "invert") || !strcmp(mode, "swap") ? PAD_INVERT
          : !strcmp(mode, "lag") ? PAD_LAG : !strcmp(mode, "drunk") ? PAD_DRUNK : -1;
    if (delay < 0) delay = m == PAD_LAG ? 15 : 10;
    if (!cyc_tcp_long(line, "buttons", &buttons) || buttons <= 0 || buttons > 0xFF || m < 0 || frames < 1 ||
        delay < 1 || delay >= LAG_MAX) {
        cyc_tcp_err(id, "chat_pad: buttons (A = 0x80 ... Right = 0x01), mode press|tap|block|swap|lag|drunk, frames, "
                        "delay (lag: frames late; drunk: frames per lurch), label");
        return;
    }
    int i = pad_slot(label);
    if (i < 0) { cyc_tcp_err(id, "chat_pad: every input effect slot is busy"); return; }
    PadFx *fx = &s_pad[i];
    fx->on = true;
    fx->mode = m;
    fx->delay = (int)delay;
    if (m == PAD_DRUNK) s_drunk_seed = SDL_GetTicks() * 2246822519u;
    fx->buttons = (uint8_t)buttons;
    fx->left = frames > 36000 ? 36000 : frames;
    snprintf(fx->label, sizeof(fx->label), "%s", label);
    char f[32];
    snprintf(f, sizeof(f), "\"slot\":%d", i);
    cyc_tcp_ok(id, f);
}

static void tcp_ram(int id, const char *line)
{
    RamFx fx;
    memset(&fx, 0, sizeof(fx));
    long addr = -1, value = 0, lo = 0, hi = -1, frames = 0, every = 1, if_addr = -1, if_eq = 0, len = 1;
    int op = -1;
    if (cyc_tcp_long(line, "set", &value)) op = RAM_SET;
    else if (cyc_tcp_long(line, "add", &value)) op = RAM_ADD;
    else if (cyc_tcp_long(line, "or", &value)) op = RAM_OR;
    else if (cyc_tcp_long(line, "and", &value)) op = RAM_AND;
    char enc[16] = "bin";
    cyc_tcp_str(line, "enc", enc, sizeof(enc));
    int e = !strcmp(enc, "bin") ? ENC_BIN : !strcmp(enc, "bin_be") ? ENC_BIN_BE : !strcmp(enc, "bcd") ? ENC_BCD
          : !strcmp(enc, "digits") ? ENC_DIGITS : -1;
    cyc_tcp_long(line, "len", &len);
    int max_len = e == ENC_DIGITS ? 8 : 4;
    /* the largest number the encoding holds in len bytes */
    long top = 0;
    for (long i = 0; i < len && i < max_len; ++i)
        top = e == ENC_DIGITS ? top * 10 + 9 : e == ENC_BCD ? top * 100 + 99 : top << 8 | 0xFF;
    if (len == 4 && (e == ENC_BIN || e == ENC_BIN_BE)) top = 0x7FFFFFFF;
    cyc_tcp_long(line, "min", &lo);
    if (!cyc_tcp_long(line, "max", &hi)) hi = top;
    cyc_tcp_long(line, "frames", &frames);
    cyc_tcp_long(line, "every", &every);
    bool cond = cyc_tcp_long(line, "if_addr", &if_addr);
    if (cond && !cyc_tcp_long(line, "if_eq", &if_eq)) if_addr = -2;
    cyc_tcp_str(line, "label", fx.label, sizeof(fx.label));
    printable(fx.label);
    uint8_t probe;
    bool bits = op == RAM_OR || op == RAM_AND;
    if (!cyc_tcp_long(line, "addr", &addr) || addr < 0 || addr + len > 0x10000 || op < 0 || e < 0 || len < 1 ||
        len > max_len || (bits && (len != 1 || value < 0 || value > 255)) || value < -top || value > top ||
        lo < 0 || hi > top || lo > hi || frames < 0 || every < 1 || if_addr < -1 || if_addr > 0xFFFF ||
        !cyc_mod_peek_ok((uint16_t)addr, &probe)) {
        cyc_tcp_err(id, "chat_ram: addr (RAM), set|add|or|and, enc bin|bin_be|bcd|digits, len, min, max, frames, "
                        "every, if_addr + if_eq, label");
        return;
    }
    fx.addr = (uint16_t)addr;
    fx.op = op;
    fx.enc = e;
    fx.len = (int)len;
    fx.value = value;
    fx.lo = lo;
    fx.hi = hi;
    fx.every = every;
    fx.if_addr = if_addr;
    fx.if_eq = if_eq;
    if (!frames) {                          /* once, now: requests are read between frames */
        ram_store(&fx);
        cyc_tcp_ok(id, NULL);
        return;
    }
    int i = ram_slot(fx.label);
    if (i < 0) { cyc_tcp_err(id, "chat_ram: every RAM effect slot is busy"); return; }
    fx.on = true;
    fx.left = frames > 36000 ? 36000 : frames;
    s_ram[i] = fx;
    char f[32];
    snprintf(f, sizeof(f), "\"slot\":%d", i);
    cyc_tcp_ok(id, f);
}

static long clamp_ms(long ms, long lo, long hi) { return ms < lo ? lo : ms > hi ? hi : ms; }

static void tcp_fx(int id, const char *line)
{
    char kind[16] = "", label[40] = "";
    long ms = 10000, amount = 0, x = 50, y = 50;
    cyc_tcp_str(line, "effect", kind, sizeof(kind));
    cyc_tcp_str(line, "label", label, sizeof(label));
    printable(label);
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_long(line, "amount", &amount);
    cyc_tcp_long(line, "x", &x);
    cyc_tcp_long(line, "y", &y);
    int k = 0;
    while (k < FX_KINDS && strcmp(kind, FX_NAMES[k])) ++k;
    if (k == FX_KINDS) {
        char e[256] = "chat_fx: effect ";
        for (int i = 0; i < FX_KINDS; ++i) { strcat(e, FX_NAMES[i]); strcat(e, i + 1 < FX_KINDS ? "|" : ""); }
        strcat(e, ", ms (0 ends it), amount, color, x, y, label");
        cyc_tcp_err(id, e);
        return;
    }
    PicFx *f = &s_fx[k];
    if (ms <= 0) { f->on = false; cyc_tcp_ok(id, NULL); return; }
    f->on = true;
    f->amount = amount;
    f->color = color_field(line, "color", 0xFF4040);
    f->x = (int)clamp_ms(x, 0, 100);
    f->y = (int)clamp_ms(y, 0, 100);
    f->born = SDL_GetTicks();
    f->life = (uint32_t)clamp_ms(ms, 100, 600000);
    snprintf(f->label, sizeof(f->label), "%s", label);
    cyc_tcp_ok(id, NULL);
}

/* The clock: mode slow|fast (percent of full speed), stop (no frames run). */
static void tcp_time(int id, const char *line)
{
    char mode[16] = "", label[40] = "";
    long ms = 5000, percent = 50;
    cyc_tcp_str(line, "mode", mode, sizeof(mode));
    cyc_tcp_str(line, "label", label, sizeof(label));
    printable(label);
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_long(line, "percent", &percent);
    if (online()) { cyc_tcp_err(id, "chat_time: not during online play"); return; }
    bool stop = !strcmp(mode, "stop");
    if (!stop && strcmp(mode, "speed")) {
        cyc_tcp_err(id, "chat_time: mode speed (percent 10..400) | stop, ms (0 ends it), label");
        return;
    }
    if (stop) {
        s_stop.on = ms > 0;
        s_stop.born = SDL_GetTicks();
        s_stop.life = (uint32_t)clamp_ms(ms, 100, 60000);
        snprintf(s_stop.label, sizeof(s_stop.label), "%s", label);
    } else {
        s_speed.on = ms > 0 && percent != 100;
        s_speed.percent = (int)clamp_ms(percent, 10, 400);
        s_speed.born = SDL_GetTicks();
        s_speed.life = (uint32_t)clamp_ms(ms, 100, 600000);
        snprintf(s_speed.label, sizeof(s_speed.label), "%s", label);
    }
    cyc_tcp_ok(id, NULL);
}

static void tcp_audio(int id, const char *line)
{
    long mask = 0x3F, ms = 5000;
    char label[40] = "";
    cyc_tcp_long(line, "mute", &mask);
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_str(line, "label", label, sizeof(label));
    printable(label);
    if (mask < 0 || mask > 0x3F) { cyc_tcp_err(id, "chat_audio: mute (bits: pulse 1, pulse 2, triangle, noise, DMC, cartridge), ms, label"); return; }
    s_mute.on = ms > 0 && mask;
    s_mute.mask = (uint8_t)mask;
    s_mute.born = SDL_GetTicks();
    s_mute.life = (uint32_t)clamp_ms(ms, 100, 600000);
    snprintf(s_mute.label, sizeof(s_mute.label), "%s", label);
    cyc_tcp_ok(id, NULL);
}

/* Rewind: `on` keeps a snapshot every half second (the last twelve seconds);
 * `seconds` goes back that far (as far as the snapshots reach). */
static void tcp_rewind(int id, const char *line)
{
    bool on;
    if (cyc_tcp_bool(line, "on", &on)) {
        s_rewind_on = on;
        if (!on) rewind_clear();
    }
    long seconds = 0;
    cyc_tcp_long(line, "seconds", &seconds);
    char f[96];
    if (seconds > 0) {
        if (online()) { cyc_tcp_err(id, "chat_rewind: not during online play"); return; }
        long back = seconds * 60;
        int i = s_rewind_count - 1;
        while (i > 0 && s_rewind_frames - s_rewind[i].frame < back) --i;
        if (i < 0) { cyc_tcp_err(id, s_rewind_on ? "chat_rewind: nothing recorded yet" : "chat_rewind: switch it on first (on true)"); return; }
        char err[160];
        long went = (s_rewind_frames - s_rewind[i].frame) / 60;
        if (!state_apply(s_rewind[i].data, s_rewind[i].len, err, sizeof(err))) { cyc_tcp_err(id, err); return; }
        for (int k = i + 1; k < s_rewind_count; ++k) snap_free(&s_rewind[k].data);
        s_rewind_count = i + 1;
        s_rewind_frames = s_rewind[i].frame;
        snprintf(f, sizeof(f), "\"on\":%s,\"went_back_s\":%ld", s_rewind_on ? "true" : "false", went);
    } else {
        long reach = s_rewind_count ? (s_rewind_frames - s_rewind[0].frame) / 60 : 0;
        snprintf(f, sizeof(f), "\"on\":%s,\"reach_s\":%ld", s_rewind_on ? "true" : "false", reach);
    }
    cyc_tcp_ok(id, f);
}

/* One checkpoint chat can come back to: do save | load. */
static void tcp_checkpoint(int id, const char *line)
{
    char what[16] = "", err[160];
    cyc_tcp_str(line, "do", what, sizeof(what));
    if (online()) { cyc_tcp_err(id, "chat_checkpoint: not during online play"); return; }
    if (!strcmp(what, "save")) {
        uint8_t *data;
        size_t len;
        if (!cyc_state_save(&data, &len, err, sizeof(err))) { cyc_tcp_err(id, err); return; }
        snap_free(&s_checkpoint.data);
        s_checkpoint.data = data;
        s_checkpoint.len = len;
    } else if (!strcmp(what, "load")) {
        if (!s_checkpoint.data) { cyc_tcp_err(id, "chat_checkpoint: none saved"); return; }
        if (!state_apply(s_checkpoint.data, s_checkpoint.len, err, sizeof(err))) { cyc_tcp_err(id, err); return; }
        rewind_clear();
    } else {
        cyc_tcp_err(id, "chat_checkpoint: do save|load");
        return;
    }
    cyc_tcp_ok(id, NULL);
}

/* What is running: the ROM file the window opened and the PRG hash the
 * compiled program was made from, for a bridge to pick its game file. */
static void tcp_rom_info(int id, const char *line)
{
    (void)line;
    char q[1100], f[1300];
    cyc_tcp_quote(s_host.rom_path ? s_host.rom_path() : "", q, sizeof(q));
    snprintf(f, sizeof(f), "\"rom\":%s,\"prg_fnv1a\":\"%08X\"", q, (unsigned)cyc_prg_hash());
    cyc_tcp_ok(id, f);
}

/* One of the program's own routines, between frames, its RAM effects kept
 * (cyc_mod_call_commit): what a RAM effect alone would leave half done. */
static void tcp_call(int id, const char *line)
{
    long addr = -1, a = 0, x = 0, y = 0;
    cyc_tcp_long(line, "a", &a);
    cyc_tcp_long(line, "x", &x);
    cyc_tcp_long(line, "y", &y);
    if (!cyc_tcp_long(line, "addr", &addr) || addr < 0 || addr > 0xFFFF) {
        cyc_tcp_err(id, "chat_call: addr, a, x, y");
        return;
    }
    CycModRegs r;
    cyc_mod_regs(&r);
    r.a = (uint8_t)a;
    r.x = (uint8_t)x;
    r.y = (uint8_t)y;
    if (!cyc_mod_call_commit((uint16_t)addr, &r)) { cyc_tcp_err(id, "chat_call: the routine did not return"); return; }
    cyc_tcp_ok(id, NULL);
}

static void tcp_config(int id, const char *line)
{
    char side[16], valign[16];
    long lines = s_cfg.lines, ms = (long)s_cfg.ms, scale = s_cfg.scale, offset = s_cfg.offset;
    cyc_tcp_bool(line, "show", &s_cfg.show);
    if (cyc_tcp_str(line, "side", side, sizeof(side))) s_cfg.right = !strcmp(side, "right");
    if (cyc_tcp_str(line, "valign", valign, sizeof(valign))) s_cfg.top = !strcmp(valign, "top");
    cyc_tcp_long(line, "lines", &lines);
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_long(line, "scale", &scale);
    cyc_tcp_long(line, "offset", &offset);
    s_cfg.lines = lines < 1 ? 1 : lines > ROWS_MAX ? ROWS_MAX : (int)lines;
    s_cfg.ms = (uint32_t)(ms < 500 ? 500 : ms > 600000 ? 600000 : ms);
    s_cfg.scale = scale < 0 ? 0 : scale > 8 ? 8 : (int)scale;
    s_cfg.offset = offset < 0 ? 0 : offset > 80 ? 80 : (int)offset;
    char f[192];
    snprintf(f, sizeof(f), "\"show\":%s,\"side\":\"%s\",\"valign\":\"%s\",\"offset\":%d,\"lines\":%d,\"ms\":%u,\"scale\":%d",
             s_cfg.show ? "true" : "false", s_cfg.right ? "right" : "left", s_cfg.top ? "top" : "bottom", s_cfg.offset,
             s_cfg.lines, (unsigned)s_cfg.ms, s_cfg.scale);
    cyc_tcp_ok(id, f);
}

static void tcp_clear(int id, const char *line)
{
    (void)line;
    s_msg_count = s_msg_next = 0;
    s_banner.life = 0;
    s_poll.count = 0;
    memset(s_pad, 0, sizeof(s_pad));
    memset(s_ram, 0, sizeof(s_ram));
    memset(s_fx, 0, sizeof(s_fx));
    s_speed.on = s_stop.on = s_mute.on = false;
    cyc_tcp_ok(id, NULL);
}

static void tcp_state(int id, const char *line)
{
    (void)line;
    uint32_t now = SDL_GetTicks();
    int shown = 0, pads = 0, rams = 0;
    for (int i = 0; i < s_msg_count; ++i)
        if (now - s_msg[i].born < s_msg[i].life) shown++;
    for (int i = 0; i < PAD_MAX; ++i) pads += s_pad[i].on;
    for (int i = 0; i < RAM_MAX; ++i) rams += s_ram[i].on;
    char f[160];
    snprintf(f, sizeof(f), "\"messages\":%d,\"banner\":%s,\"pad_effects\":%d,\"ram_effects\":%d,\"show\":%s", shown,
             s_banner.life && now - s_banner.born < s_banner.life ? "true" : "false", pads, rams,
             s_cfg.show ? "true" : "false");
    cyc_tcp_ok(id, f);
}

void cyc_chat_register_tcp(void)
{
    cyc_tcp_register("chat", "a chat line over the game: user, text, color (RRGGBB), ms", tcp_chat);
    cyc_tcp_register("chat_banner", "a big line across the picture: text, color, ms", tcp_banner);
    cyc_tcp_register("chat_poll", "a poll over the game: title, options a|b|c, votes 1|0|2, ms, countdown, winner, y (%)", tcp_poll);
    cyc_tcp_register("chat_pad", "player 1 input effect: buttons, mode press|tap|block|invert, frames, label", tcp_pad);
    cyc_tcp_register("chat_ram", "RAM effect: addr, set|add|or|and, min, max, frames, every, if_addr, if_eq, label", tcp_ram);
    cyc_tcp_register("chat_fx", "a picture effect: effect, ms, amount, color, x, y, label", tcp_fx);
    cyc_tcp_register("chat_time", "the clock: mode speed (percent) | stop, ms, label", tcp_time);
    cyc_tcp_register("chat_audio", "mute channels: mute (bit mask), ms, label", tcp_audio);
    cyc_tcp_register("chat_rewind", "snapshots for rewinding: on, seconds", tcp_rewind);
    cyc_tcp_register("chat_checkpoint", "one checkpoint: do save|load", tcp_checkpoint);
    cyc_tcp_register("rom_info", "the ROM file and PRG hash running", tcp_rom_info);
    cyc_tcp_register("chat_call", "run a routine of the program now and keep its RAM effects: addr, a, x, y", tcp_call);
    cyc_tcp_register("chat_config", "the chat panel: show, side left|right, valign top|bottom, offset (% of height), lines, ms, scale", tcp_config);
    cyc_tcp_register("chat_clear", "drop chat lines, the banner and every chat effect", tcp_clear);
    cyc_tcp_register("chat_state", "chat lines up, banner, active effects", tcp_state);
}

/* ---- drawing, into a buffer of chat pixels scaled up by whole window pixels ---- */

static void blend(int x, int y, uint32_t color, unsigned alpha)
{
    if (x < 0 || y < 0 || x >= s_tex_w || y >= s_tex_h || !alpha) return;
    uint32_t *p = &s_px[(size_t)y * (size_t)s_tex_w + (size_t)x];
    unsigned da = *p >> 24;
    if (alpha >= 255 || !da) { *p = (uint32_t)alpha << 24 | color; return; }
    unsigned oa = alpha + da * (255 - alpha) / 255, r = 0;
    for (int s = 0; s < 24; s += 8) {
        unsigned sc = (color >> s) & 255, dc = (*p >> s) & 255;
        r |= ((sc * alpha + dc * da * (255 - alpha) / 255) / oa) << s;
    }
    *p = (uint32_t)oa << 24 | r;
}

static void box(int x, int y, int w, int h, uint32_t color, unsigned alpha)
{
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) blend(xx, yy, color, alpha);
}

static void glyph(int x, int y, int zoom, char c, uint32_t color, unsigned alpha)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = CHAT_FONT[c - 0x20];
    for (int r = 0; r < 8; ++r)
        for (int b = 0; b < 8; ++b)
            if (g[r] >> b & 1) box(x + b * zoom, y + r * zoom, zoom, zoom, color, alpha);
}

/* Text with a one-pixel dark edge below and right (the panel's rows) or all
 * round (the banner, which sits on the picture itself). */
static void text(int x, int y, int zoom, const char *s, int n, uint32_t color, unsigned alpha, bool outline)
{
    for (int i = 0; i < n; ++i) {
        int gx = x + i * 8 * zoom;
        if (outline) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy) glyph(gx + dx * zoom, y + dy * zoom, zoom, s[i], 0x000000, alpha);
        } else {
            glyph(gx + 1, y + 1, zoom, s[i], 0x000000, alpha * 3 / 4);
        }
        glyph(gx, y, zoom, s[i], color, alpha);
    }
}

static unsigned fade(uint32_t now, uint32_t born, uint32_t life)
{
    uint32_t age = now - born;
    if (age >= life) return 0;
    uint32_t left = life - age;
    return left >= FADE_MS ? 255 : 255 * left / FADE_MS;
}

typedef struct { const Msg *m; int at, len; unsigned alpha; } Row;

/* "user: text" word-wrapped into rows of `cols`, newest message at the bottom. */
static int wrap(const Msg *m, int cols, unsigned alpha, Row *out, int max)
{
    char all[300];
    int n = snprintf(all, sizeof(all), m->user[0] ? "%s: %s" : "%s%s", m->user, m->text);
    if (n < 0) return 0;
    if (n >= (int)sizeof(all)) n = (int)sizeof(all) - 1;
    Row rows[40];
    int count = 0;
    for (int at = 0; at < n && count < 40;) {
        int len = n - at;
        if (len > cols) {
            len = cols;
            for (int k = cols; k > cols / 2; --k)
                if (all[at + k] == ' ') { len = k; break; }
        }
        rows[count++] = (Row){ m, at, len, alpha };
        at += len;
        while (at < n && all[at] == ' ') ++at;
    }
    if (count > max) count = max;            /* a very long message keeps its start */
    memcpy(out, rows, sizeof(Row) * (size_t)count);
    return count;
}

static void draw_panel(uint32_t now, int lw, int lh)
{
    int cols = lw * 2 / 5 / 8;
    if (cols < 16) cols = 16;
    if (cols > 48) cols = 48;
    Row rows[ROWS_MAX];
    int used = 0;
    /* newest first, so the newest message always has room */
    for (int k = 0; k < s_msg_count && used < s_cfg.lines; ++k) {
        const Msg *m = &s_msg[(s_msg_next - 1 - k + MSG_MAX) % MSG_MAX];
        unsigned a = fade(now, m->born, m->life);
        if (!a) continue;
        Row mine[ROWS_MAX];
        int n = wrap(m, cols, a, mine, s_cfg.lines - used);
        memmove(rows + n, rows, sizeof(Row) * (size_t)used);
        memcpy(rows, mine, sizeof(Row) * (size_t)n);
        used += n;
    }
    if (!used) return;
    int edge = lh * s_cfg.offset / 100 + 2;
    int x0 = s_cfg.right ? lw - 6 - cols * 8 - 4 : 6, y = s_cfg.top ? edge : lh - edge - used * LINE_H;
    for (int i = 0; i < used; ++i, y += LINE_H) {
        const Row *r = &rows[i];
        const Msg *m = r->m;
        char all[300];
        snprintf(all, sizeof(all), m->user[0] ? "%s: %s" : "%s%s", m->user, m->text);
        int name = m->user[0] ? (int)strlen(m->user) + 1 : 0;
        box(x0 - 2, y - 1, r->len * 8 + 4, LINE_H, 0x0C0C14, r->alpha * 150 / 255);
        int split = name - r->at;
        if (split < 0) split = 0;
        if (split > r->len) split = r->len;
        text(x0, y, 1, all + r->at, split, m->color, r->alpha, false);
        text(x0 + split * 8, y, 1, all + r->at + split, r->len - split, m->user[0] ? 0xF0F0F0 : m->color, r->alpha, false);
    }
}

#define TIMERS_MAX 16

/* Every labelled effect running, as "LABEL 12s": frame-counted ones (input,
 * RAM) and wall-clock ones (picture, clock, sound). */
static int timers(uint32_t now, char rows[TIMERS_MAX][64])
{
    int n = 0;
    for (int i = 0; i < PAD_MAX && n < TIMERS_MAX; ++i)
        if (s_pad[i].on && s_pad[i].label[0]) snprintf(rows[n++], 64, "%s %lds", s_pad[i].label, (s_pad[i].left + 59) / 60);
    for (int i = 0; i < RAM_MAX && n < TIMERS_MAX; ++i)
        if (s_ram[i].on && s_ram[i].label[0]) snprintf(rows[n++], 64, "%s %lds", s_ram[i].label, (s_ram[i].left + 59) / 60);
    typedef struct { bool on; const char *label; uint32_t born, life; } Clocked;
    Clocked clock[FX_KINDS + 3];
    int c = 0;
    for (int k = 0; k < FX_KINDS; ++k) clock[c++] = (Clocked){ s_fx[k].on, s_fx[k].label, s_fx[k].born, s_fx[k].life };
    clock[c++] = (Clocked){ s_speed.on, s_speed.label, s_speed.born, s_speed.life };
    clock[c++] = (Clocked){ s_stop.on, s_stop.label, s_stop.born, s_stop.life };
    clock[c++] = (Clocked){ s_mute.on, s_mute.label, s_mute.born, s_mute.life };
    for (int i = 0; i < c && n < TIMERS_MAX; ++i)
        if (clock[i].on && clock[i].label[0] && live(now, clock[i].born, clock[i].life))
            snprintf(rows[n++], 64, "%s %us", clock[i].label, (unsigned)((clock[i].life - (now - clock[i].born) + 999) / 1000));
    return n;
}

static void draw_effects(uint32_t now, int lw)
{
    char rows[TIMERS_MAX][64];
    int count = timers(now, rows), y = 4;
    for (int i = 0; i < count; ++i, y += LINE_H + 3) {
        int n = (int)strlen(rows[i]), x = lw - 6 - n * 8;
        box(x - 3, y - 2, n * 8 + 6, LINE_H + 2, 0x241A04, 190);
        text(x, y, 1, rows[i], n, 0xFFD25A, 255, false);
    }
}

static void draw_banner(uint32_t now, int lw, int lh)
{
    unsigned a = s_banner.life ? fade(now, s_banner.born, s_banner.life) : 0;
    if (!a) return;
    int n = (int)strlen(s_banner.text), zoom = 2;
    while (zoom > 1 && n * 8 * zoom > lw - 8) --zoom;
    if (n * 8 * zoom > lw - 8) n = (lw - 8) / (8 * zoom);
    int x = (lw - n * 8 * zoom) / 2, y = lh / 3 - 4 * zoom;
    text(x, y, zoom, s_banner.text, n, s_banner.color, a, true);
}

/* The poll: a title with its countdown, then one row per option with its
 * number, a bar for its share of the votes and its count; the winner lit. */
static void draw_poll(uint32_t now, int lw, int lh)
{
    unsigned a = s_poll.life ? fade(now, s_poll.born, s_poll.life) : 0;
    if (!a || !s_poll.count) return;
    const int cols = 30, w = cols * 8 + 8;
    int total = 0, best = 0;
    for (int i = 0; i < s_poll.count; ++i) {
        total += s_poll.votes[i];
        if (s_poll.votes[i] > best) best = s_poll.votes[i];
    }
    int x = (lw - w) / 2, y = lh * s_poll.y / 100;
    box(x, y, w, (s_poll.count + 1) * (LINE_H + 2) + 6, 0x0C0C14, a * 190 / 255);
    char row[64];
    int n;
    if (s_poll.countdown) {
        uint32_t left = s_poll.life - (now - s_poll.born);
        n = snprintf(row, sizeof(row), "%.22s %us", s_poll.title, (unsigned)((left + 999) / 1000));
    } else {
        n = snprintf(row, sizeof(row), "%.29s", s_poll.title);
    }
    text(x + 4, y + 3, 1, row, n < cols ? n : cols, 0xFFD25A, a, false);
    for (int i = 0; i < s_poll.count; ++i) {
        int ry = y + 3 + (i + 1) * (LINE_H + 2);
        bool lit = s_poll.winner == i || (s_poll.winner < 0 && best && s_poll.votes[i] == best);
        int bar = total ? s_poll.votes[i] * (w - 8) / total : 0;
        box(x + 4, ry - 1, bar, LINE_H, s_poll.winner == i ? 0x3C8C3C : 0x2A3A6A, a * 220 / 255);
        n = snprintf(row, sizeof(row), "%d %-20.20s %4d", i + 1, s_poll.option[i], s_poll.votes[i]);
        text(x + 4, ry, 1, row, n < cols ? n : cols, lit ? 0xFFFFFF : 0xB4B4C8, a, false);
    }
}

static bool anything(uint32_t now)
{
    if (s_banner.life && now - s_banner.born < s_banner.life) return true;
    if (s_poll.life && s_poll.count && now - s_poll.born < s_poll.life) return true;
    char rows[TIMERS_MAX][64];
    if (timers(now, rows)) return true;
    if (!s_cfg.show) return false;
    for (int i = 0; i < s_msg_count; ++i)
        if (now - s_msg[i].born < s_msg[i].life) return true;
    return false;
}

void cyc_chat_draw(SDL_Renderer *ren)
{
    uint32_t now = SDL_GetTicks();
    if (!anything(now)) return;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    int zoom = s_cfg.scale ? s_cfg.scale : (oh + 180) / 360;
    if (zoom < 1) zoom = 1;
    int lw = ow / zoom, lh = oh / zoom;
    if (lw < 64 || lh < 64) return;
    if (!s_tex || lw != s_tex_w || lh != s_tex_h) {
        if (s_tex) SDL_DestroyTexture(s_tex);
        s_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, lw, lh);
        if (!s_tex) return;
        SDL_SetTextureBlendMode(s_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(s_tex, SDL_ScaleModeNearest);
        s_tex_w = lw;
        s_tex_h = lh;
    }
    size_t need = (size_t)lw * (size_t)lh;
    if (need > s_px_len) {
        free(s_px);
        s_px = (uint32_t *)malloc(need * 4);
        s_px_len = s_px ? need : 0;
        if (!s_px) return;
    }
    memset(s_px, 0, need * 4);
    if (s_cfg.show) draw_panel(now, lw, lh);
    draw_effects(now, lw);
    draw_poll(now, lw, lh);
    draw_banner(now, lw, lh);
    SDL_UpdateTexture(s_tex, NULL, s_px, lw * 4);

    int llw = 0, llh = 0;
    SDL_RenderGetLogicalSize(ren, &llw, &llh);
    SDL_bool integer = SDL_RenderGetIntegerScale(ren);
    SDL_RenderSetLogicalSize(ren, 0, 0);                  /* window pixels */
    SDL_RenderSetViewport(ren, NULL);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    SDL_Rect dst = { 0, 0, lw * zoom, lh * zoom };
    SDL_RenderCopy(ren, s_tex, NULL, &dst);
    SDL_RenderSetLogicalSize(ren, llw, llh);              /* back to the picture's coordinates */
    SDL_RenderSetIntegerScale(ren, integer);
}
