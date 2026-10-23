#!/usr/bin/env node
// twitch_bridge.mjs - Twitch chat into a running nesrecomp game window.
//
//   node twitch_bridge.mjs --channel NAME --port 4370
//   node twitch_bridge.mjs --stdin --port 4371 --mode vote
//
// Start the game with --tcp PORT (either order works: both sides retry).
// The bridge asks the window which ROM runs and builds its commands from
// layers: the universal library (any game), the semantic library (for the
// vars the game file declares), the game file whose "match" names the ROM
// (games/ here and any --games DIR), then --settings. An unknown ROM gets
// the universal commands, and its hashes are logged for a new game file.
//
// --channel  the Twitch channel to read (anonymous, read-only: no token)
// --stdin    type chat yourself: "name: text" (as a viewer) or "text" (as the broadcaster)
// --both     Twitch and stdin together
// --mode     anarchy | vote | both, over every other layer's vote.mode
// --game     use this game file whatever the ROM
// --games    another directory of game files (repeatable)
// --settings the streamer's own layer: disable, tweak, cooldowns, chat, vote
// See README.md for the formats.

import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';
import { connectGame } from './game.mjs';
import { createEngine, layerProfile } from './engine.mjs';
import { identify, loadGames } from './identify.mjs';
import { connectTwitch } from './irc.mjs';
import { semantic } from './library/semantic.mjs';
import { universal } from './library/universal.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));

function args(argv) {
  const out = { port: 4370, stdin: false, games: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => {
      if (i + 1 >= argv.length) throw new Error(`${a} needs a value`);
      return argv[++i];
    };
    if (a === '--channel') out.channel = val();
    else if (a === '--port') out.port = Number(val());
    else if (a === '--stdin') out.stdin = true;
    else if (a === '--both') out.both = true;
    else if (a === '--mode') out.mode = val();
    else if (a === '--game' || a === '--profile') out.game = val();
    else if (a === '--games') out.games.push(val());
    else if (a === '--settings') out.settings = val();
    else if (a === '--help' || a === '-h') out.help = true;
    else throw new Error(`unknown argument ${a}`);
  }
  return out;
}

function usage() {
  console.log(`usage: node twitch_bridge.mjs (--channel NAME | --stdin | --channel NAME --both) [--port 4370]
       [--mode anarchy|vote|both] [--game FILE] [--games DIR]... [--settings FILE]`);
}

const readJson = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));

// The game file for this window, or null (universal commands only).
async function gameFor(game, o, log) {
  if (o.game) {
    const file = fs.existsSync(o.game) ? o.game : path.join(here, 'games', o.game);
    log(`game file (forced): ${file}`);
    return readJson(file);
  }
  const info = await game.send('rom_info');
  const { identity, entry } = identify(info, loadGames([path.join(here, 'games'), ...o.games]));
  if (identity.error) log(identity.error);
  if (entry) {
    log(`${entry.game.name}: ${path.relative(process.cwd(), entry.file)} (CRC32 ${identity.crc32 ?? '?'})`);
    return entry.game;
  }
  log(`no game file names this ROM - universal commands only. To add one, start a games/*.json with:`);
  log(`  "match": { "crc32": ["${identity.crc32 ?? ''}"], "sha1": ["${identity.sha1 ?? ''}"], "prg_fnv1a": ["${identity.prg_fnv1a}"] }`);
  return null;
}

function main() {
  let o;
  try {
    o = args(process.argv.slice(2));
  } catch (e) {
    console.error(e.message);
    usage();
    process.exit(2);
  }
  if (o.help || (!o.channel && !o.stdin)) {
    usage();
    process.exit(o.help ? 0 : 2);
  }
  const settings = o.settings ? readJson(o.settings) : null;
  const log = (s) => console.log(`[${new Date().toLocaleTimeString()}] ${s}`);

  let engine = null;
  let stopVotes = () => {};
  // On every (re)connect: which game, then a fresh engine for it.
  const start = async () => {
    stopVotes();
    engine = null;
    const gameFile = await gameFor(game, o, log);
    const profile = layerProfile([universal, semantic, gameFile, settings, o.mode && { vote: { mode: o.mode } }]);
    const e = createEngine(profile, { game, log });
    await e.configure();
    engine = e;
    const v = e.profile.vote;
    log(`${e.profile.name}: ${e.profile.commands.size} commands, vars: ${Object.keys(e.profile.vars).join(', ') || 'none'}`);
    log(`voting: ${v.mode}${v.mode === 'anarchy' ? ' (mods: !vote on)' : ''}, ${v.options} options, ` +
        `${v.durationMs / 1000}s rounds every ${(v.durationMs + v.gapMs) / 1000}s`);
    stopVotes = e.runVotes();
  };
  const game = connectGame({
    port: o.port,
    onStatus: (s) => {
      log(s);
      if (game?.connected) start().catch((e) => log(`setup: ${e.message}`));
    },
  });

  const feed = (msg) => {
    if (game.connected && engine) engine.handle(msg); // else nothing to show it on yet
  };

  let irc = null;
  if (o.channel) irc = connectTwitch({ channel: o.channel, onMessage: feed, onStatus: (s) => log(`twitch: ${s}`) });
  if (o.stdin || o.both) {
    const rl = readline.createInterface({ input: process.stdin });
    rl.on('line', (line) => {
      const m = /^([A-Za-z0-9_]{1,25}):\s*(.*)$/.exec(line);
      const viewer = !!m;
      const user = m ? m[1] : 'broadcaster';
      feed({ user, login: user.toLowerCase(), text: m ? m[2] : line, color: '', bits: 0, rewardId: '',
             broadcaster: !viewer, mod: false, vip: false, sub: false });
    });
    rl.on('close', () => { if (!irc) setTimeout(() => process.exit(0), 500); });
  }
  process.on('SIGINT', () => {
    stopVotes();
    irc?.close();
    game.close();
    process.exit(0);
  });
}

main();
