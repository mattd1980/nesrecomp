/*
 * cyc_chat.h - a live chat shown over the game and allowed to steer it.
 *
 * The window learns about chat only through its TCP server (cyc_tcp.h); a
 * bridge such as tools/twitch/twitch_bridge.mjs reads a Twitch channel and
 * turns messages and commands into these requests.
 *
 * Shown over the game:
 *   chat        {user, text, color "RRGGBB", ms}   a line in the chat panel
 *   chat_banner {text, color, ms}                  a big line across the picture
 *   chat_poll   {title, options "A|B", votes "3|1", ms, countdown, winner, y}
 *               a poll panel centred at y percent of the height, up for
 *               ms (counted down in its title unless countdown false), the
 *               winner (0-based) lit; no options takes it down
 *   chat_config {show, side left|right, valign top|bottom, offset (percent
 *               of the window height from that edge), lines, ms, scale}
 *
 * Effects any game takes (they know nothing of its RAM):
 *   chat_pad    {buttons, mode, frames, delay, label}  player 1's input, per
 *               frame: press (held), tap (8 frames on, 8 off), block (masked
 *               off), swap / invert (each pair named trades places: Left/Right,
 *               Up/Down, A/B), lag (the buttons named arrive `delay` frames
 *               late), drunk (one of the buttons named, now and then)
 *   chat_fx     {effect, ms, amount, color, x, y, label}  the picture as the
 *               window shows it: flip_h, flip_v, mirror, gray, negative,
 *               rainbow, tint (color), pixelate (block), shake (pixels), zoom
 *               (percent), wobble (pixels), spotlight (radius % of the height
 *               at x,y %); one of each kind at a time, ms 0 ends it
 *   chat_time   {mode speed|stop, percent, ms, label}  frames run at percent
 *               of full speed, or none at all
 *   chat_audio  {mute, ms, label}                  channels silenced (bits: pulse
 *               1, pulse 2, triangle, noise, DMC, cartridge); the machine runs on
 *   chat_rewind {on, seconds}                      on keeps a snapshot every half
 *               second (twelve seconds of them); seconds goes back that far
 *   chat_checkpoint {do save|load}                 one state chat can return to
 *
 * Effects that know the game (a bridge's game file says where things are):
 *   chat_ram    {addr, set|add|or|and, enc, len, min, max, frames, every,
 *               if_addr, if_eq, label}             a number stored as the CPU
 *               would store it (cyc_mod_poke), once now or again every `every`
 *               frames for `frames` frames. enc: bin (len 1-4, little-endian),
 *               bin_be, bcd (len 1-4, most significant byte first), digits (one
 *               decimal digit per byte, len 1-8, most significant first); set
 *               and add work in those units and stay within min..max; or/and
 *               work on one byte's bits. if_addr/if_eq skip the store while
 *               RAM[if_addr] != if_eq
 *   chat_call   {addr, a, x, y}                    one of the program's routines,
 *               now, its RAM effects kept (cyc_mod_call_commit)
 *
 *   rom_info    {}                                 the ROM file and PRG hash running
 *   chat_clear  {}                                 messages, banner, poll and every effect
 *   chat_state  {}                                 what is up and active
 *
 * A labelled effect is listed with its time left; a new input or RAM effect
 * with the same label replaces it. Everything drawn here, picture effects
 * included, is presentation in window pixels: the picture itself
 * (screenshots of it, hashes, comparisons) never changes. Input, RAM, clock
 * and state effects stay off while a netplay session runs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Renderer SDL_Renderer;

/* What the window tells the chat about itself. */
typedef struct {
    bool (*online)(void);              /* a netplay session runs */
    void (*state_loaded)(void);        /* after a rewind or checkpoint load */
    const char *(*rom_path)(void);     /* the ROM file the window opened */
} CycChatHost;

void cyc_chat_set_host(const CycChatHost *host);
void cyc_chat_register_tcp(void);
/* Once per emulated frame, just before it runs: player 1's buttons after the
 * chat's input effects, with the RAM effects due this frame stored. */
uint8_t cyc_chat_frame(uint8_t pad);
/* The clock: frame time scale (2 = half speed), and whether frames run at all. */
float cyc_chat_pace(void);
bool  cyc_chat_frozen(void);
/* APU channels to silence (chat_audio's bits). */
uint8_t cyc_chat_muted(void);
/* The picture with chat's picture effects, or `pic` itself when none runs. */
const uint32_t *cyc_chat_picture(const uint32_t *pic, int w, int h);
/* Draw over everything presented so far, in window pixels (the renderer's
 * logical size, viewport, scale and blend mode are restored). */
void cyc_chat_draw(SDL_Renderer *ren);

#ifdef __cplusplus
}
#endif
