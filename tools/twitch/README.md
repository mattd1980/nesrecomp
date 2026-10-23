# Twitch chat in the game

Twitch chat shows up inside the game window, and viewers use `!commands` that
change the game. Any NES game works with no setup. A small game file adds
commands that understand that game's health, lives, timer and so on.

- **The window** (`runner/cyc/cyc_chat.c`). It draws the chat panel, a banner,
  a poll and effect timers over the picture, and it applies the effects. It
  takes `chat_*` requests on its TCP debug port (listed in `cyc_chat.h`).
- **The bridge** (`twitch_bridge.mjs`). It reads a Twitch channel anonymously
  (no account and no token, so it can only read). It asks the window which
  ROM is running, and builds that game's commands from the layers below.

## Running it

```powershell
# 1. the game, with its TCP port open (localhost only)
SuperMarioBrosRecomp.exe smb1.nes --tcp 4370
# 2. the bridge (Node 22+)
node tools/twitch/twitch_bridge.mjs --channel YOUR_CHANNEL --port 4370
```

You can start them in either order, because both sides keep retrying.

- `--stdin` lets you type chat yourself: `name: text` is sent as a viewer, and
  a plain line is sent as the broadcaster (no cooldowns). `--both` reads
  Twitch and stdin at the same time.
- `--mode anarchy|vote|both` chooses the voting mode.
- `--settings FILE` adds your own layer (see below).
- `--games DIR` adds a directory of game files.
- `--game FILE` forces one game file, whatever the ROM.

The TCP server serves one client at a time. While the bridge is connected, no
other TCP script can drive the window.

## Layers

Commands come from four layers. A later layer wins:

1. **`library/universal.mjs`** works in every game. It acts on the console,
   never on the game's RAM:
   - **Input:** `!jump`, `!left`, `!right`, `!hop`, `!reverse`, `!swapab`,
     `!lag`, `!drunk`, `!nojump`, `!stun`
   - **Picture:** `!flip`, `!upsidedown`, `!gray`, `!negative`, `!rainbow`,
     `!redalert`, `!pixel`, `!shake`, `!zoom`, `!wobble`, `!lightsout`, `!twin`
   - **Clock:** `!slowmo`, `!fast`, `!timestop`, `!rewind` (5 s back),
     `!save` (moderators) and `!load` (a checkpoint)
   - **Sound:** `!silence`, `!nobass`
   - **Chaos:** `!glitch` scrambles one RAM byte; `!rewind` undoes it
2. **`library/semantic.mjs`** is written once against what the game's RAM
   means. A game gets each command whose vars its game file declares:
   - `lives`: `!1up`
   - `health`: `!heal`, `!hurt`, `!poison`, `!regen`
   - `timer`: `!time`, `!hurry`
   - `coins`: `!rich`
   - `score`: `!score`
3. **The game file** (`games/*.json`). The bridge picks the one whose `match`
   names the running ROM. It declares vars and states, and adds the game's
   own commands. An unknown ROM gets layer 1 only, and the bridge logs the
   `match` line for a new game file.
4. **`--settings`**: the streamer's layer, for disabling commands, tweaking
   them, and setting cooldowns, chat and vote options.

A command named again in a later layer replaces the earlier one.

## Game files

```jsonc
{
  "name": "Mega Man 3",
  "match": { "crc32": ["603AAA57"], "sha1": ["6780b3fc..."] },   // No-Intro: the ROM without its header
  "conditions": { "playing": [{ "addr": "$A2", "min": "0x81" }] }, // when game commands may run
  "player_screen": { "x": { "addr": "$0360", "minus": "$FC" }, "y": "$03C0" }, // !lightsout and !zoom follow it
  "vars": {
    "health": { "addr": "$A2", "base": "0x80", "min": 0, "max": 28, "step": 4 },
    "lives":  { "addr": "$AE", "max": 9 },
    "timer":  { "addr": "$07F8", "enc": "digits", "len": 3, "max": 999 },
    "coins":  [{ "addr": "$075E", "max": 99 }, { "addr": "$07ED", "enc": "digits", "len": 2 }]
  },
  "chat": { "side": "right", "valign": "bottom", "offset": 4, "lines": 7 },
  "vote": { "mode": "both", "y": 12 },
  "aliases": { "e": "etank" },
  "commands": { "etank": { "when": "playing", "steps": [{ "ram": { "addr": "$AF", "add": 1, "max": 9 } }] } }
}
```

- **A var** is a number the game keeps. Give it one storage, or a list when
  the game keeps it twice (SMB's coin tally and its status-bar digits).
  - `enc` says how it's stored: `bin` (`len` 1-4, little-endian), `bin_be`,
    `bcd`, or `digits` (one decimal digit per byte).
  - `base` is added to the raw value (MM3 health is stored as $80 + HP).
  - `min` and `max` are in the var's own units, and so is `step`, which the
    semantic commands use as "a bit".
- **`conditions.playing`** gates every library command that says
  `when: "playing"`.
- **Steps:**
  - `var`: `set` or `add`, in the var's units. Values may be `"max"`,
    `"min"`, `"min+1"`, `"step"` or `"-step"`.
  - `ram`: `set`, `add`, `or` or `and`. Add `enc`/`len` for numbers wider
    than one byte, `frames`/`every` to hold the store, `if_addr`/`if_eq` to
    guard it, and `label` to show a timer.
  - `pad`: buttons with `press`, `tap`, `block`, `swap`, `lag` or `drunk`.
  - `fx`: a picture effect. `follow: "player"` keeps it on `player_screen`.
  - `time`: `speed` (with `percent`) or `stop`.
  - `audio`: `mute` takes a channel bit mask.
  - The rest: `rewind`, `checkpoint` (`save`/`load`), `call` (one of the
    game's routines, keeping its RAM changes), `glitch`, `banner`, `wait_ms`.
- The steps between two waits are sent in one write, so they all land in the
  same gap between two frames.
- **Commands** take `who` (`everyone`, `sub`, `vip`, `mod` or
  `broadcaster`), `bits`, `cooldown_ms`, `when`, `variants` (the first whose
  `when` holds runs), `banner`, `title` (shown on ballots), `vote: false` (to
  keep it off ballots) and `requires` (vars it needs).
- The broadcaster and moderators skip cooldowns, and so do messages that carry
  bits or redeem a channel-point reward. The bridge logs any reward id it
  doesn't know, so you can map it in `"rewards": { "<id>": "star" }`.
- `!commands` (or `!help`) lists, in the chat panel, the commands everyone
  can use.

## Settings (`--settings`)

```jsonc
{ "disable": ["glitch", "lag"], "tweak": { "rewind": { "cooldown_ms": 30000 } },
  "user_cooldown_ms": 5000, "vote": { "mode": "vote", "duration_ms": 15000 }, "chat": { "lines": 5 } }
```

## Voting

```jsonc
"vote": { "mode": "both", "options": 3, "duration_ms": 20000, "gap_ms": 15000,
          "y": 22, "title": "CHAT VOTE", "pool": ["star", "moon", "reverse"], "show_votes": false }
```

A round puts `options` commands from `pool` on a ballot. The default pool is
every command that everyone can use, that needs no bits, and that isn't
marked `vote: false`. The game shows the ballot as a poll `y` percent of the
way down the screen, with live bars and a countdown. Viewers vote with `1`,
`2`, `3`, with `!vote 2`, or with the command itself. Each viewer has one
vote, and their latest vote counts.

The option with the most votes runs, ignoring its cooldown; ties are broken
at random. If the winner can't run right now, the next option in line runs
instead. Counted votes don't show up as chat lines unless `show_votes` is
set.

- `mode`: `anarchy` means only direct commands, and is the default without a
  game file. `vote` means only votes for viewers; the broadcaster, moderators,
  bits and rewards still run commands directly. `both` runs votes and direct
  commands together.
- The broadcaster and moderators can type `!vote off`, `!vote on` or
  `!vote now` (same as `on`).

## Checks

- Tests: `node --test tools/twitch/twitch.test.mjs`.
- `smb1.json` and `mm3.json` were checked against the real games.
- Every universal effect and the semantic vars were run in SMB1:
  `prove-it/twitch/universal-live.mjs` in the nes-decomp workspace.
