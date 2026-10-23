// node --test tools/twitch/
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { buttons, compileProfile, createEngine, layerProfile, nameColor, printable } from './engine.mjs';
import { identify, loadGames, romHashes } from './identify.mjs';
import { chatMessage, parseIrcLine } from './irc.mjs';
import { semantic } from './library/semantic.mjs';
import { universal } from './library/universal.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));

test('a tagged PRIVMSG becomes a chat message', () => {
  const line = '@badge-info=;badges=moderator/1,bits/100;bits=100;color=#1E90FF;display-name=Some\\sOne;id=abc;mod=1;' +
    'subscriber=0 :someone!someone@someone.tmi.twitch.tv PRIVMSG #chan :cheer100 !star please';
  const m = chatMessage(parseIrcLine(line));
  assert.equal(m.user, 'Some One');
  assert.equal(m.login, 'someone');
  assert.equal(m.text, 'cheer100 !star please');
  assert.equal(m.bits, 100);
  assert.equal(m.mod, true);
  assert.equal(m.broadcaster, false);
  assert.equal(m.color, '#1E90FF');
});

test('/me actions lose their CTCP wrapper; PING is not chat', () => {
  const m = chatMessage(parseIrcLine(':a!a@a PRIVMSG #c :\u0001ACTION waves\u0001'));
  assert.equal(m.text, 'waves');
  assert.equal(chatMessage(parseIrcLine('PING :tmi.twitch.tv')), null);
  assert.deepEqual(parseIrcLine('PING :tmi.twitch.tv').params, ['tmi.twitch.tv']);
});

test('chat text is reduced to what the in-game font draws', () => {
  assert.equal(printable('Ça marche \u{1F389}  très   bien'), 'Ca marche tres bien');
  assert.equal(printable('x'.repeat(300)).length, 200);
  assert.equal(buttons('a+left'), 0x82);
  assert.equal(nameColor({ color: '#0000FF', login: 'x' }), '7373FF'); // too dark: lifted
  assert.equal(nameColor({ color: '#FFD700', login: 'x' }), 'FFD700');
});

test('an unknown game gets the universal library alone; every game file compiles over both libraries', () => {
  const bare = compileProfile(layerProfile([universal, semantic]));
  assert.ok(bare.commands.has('flip') && bare.commands.has('rewind') && bare.commands.has('reverse'));
  assert.ok(!bare.commands.has('heal') && !bare.commands.has('oneup'), 'semantic commands need their vars');
  for (const { file, game } of loadGames([path.join(here, 'games')])) {
    const p = compileProfile(layerProfile([universal, semantic, game]));
    assert.ok(p.commands.size > bare.commands.size, file);
    for (const n of p.vote.pool) assert.ok(p.commands.get(n).votable, `${file}: ${n}`);
  }
});

test('vars: one command, the game file says where and how the number is kept', () => {
  const game = {
    vars: {
      health: { addr: '$A2', base: '0x80', max: 28, step: 4 },
      timer: { addr: '$07F8', enc: 'digits', len: 3, max: 999 },
      coins: [{ addr: '$075E', max: 99 }, { addr: '$07ED', enc: 'digits', len: 2 }],
    },
  };
  const p = compileProfile(layerProfile([universal, semantic, game]));
  const steps = (n) => p.commands.get(n).variants[0].steps;
  assert.deepEqual(steps('heal'), [['chat_ram', { addr: 0xa2, enc: 'bin', len: 1, set: 0x9c, min: 0x80, max: 0x9c }]]);
  assert.deepEqual(steps('hurt')[0][1], { addr: 0xa2, enc: 'bin', len: 1, add: -4, min: 0x81, max: 0x9c });
  assert.deepEqual(steps('hurry')[0][1], { addr: 0x7f8, enc: 'digits', len: 3, add: -100, min: 100, max: 999 });
  assert.deepEqual(steps('rich').map(([, f]) => [f.addr, f.enc, f.add, f.max]), [[0x75e, 'bin', 10, 99], [0x7ed, 'digits', 10, 99]]);
  assert.equal(p.commands.has('oneup'), false); // no lives var
});

test('layers: a game file replaces a library command, settings disable and tweak', () => {
  const game = { commands: { jump: { title: 'HOP HOP', steps: [{ pad: { buttons: 'a', frames: 5 } }] } } };
  const settings = { disable: ['glitch', 'flip'], tweak: { shake: { cooldown_ms: 1234 } }, vote: { mode: 'vote' } };
  const p = compileProfile(layerProfile([universal, semantic, game, settings]));
  assert.equal(p.commands.get('jump').title, 'HOP HOP');
  assert.equal(p.commands.has('glitch') || p.commands.has('flip'), false);
  assert.equal(p.aliases.has('mirror'), false, 'an alias of a disabled command goes with it');
  assert.equal(p.commands.get('shake').cooldownMs, 1234);
  assert.equal(p.vote.mode, 'vote');
});

test('a ROM is known by No-Intro hashes of its body, whatever its header says', () => {
  const body = Buffer.from('NES game body bytes');
  const header = Buffer.from([0x4e, 0x45, 0x53, 0x1a, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  const dir = fs.mkdtempSync(path.join(fs.realpathSync(os.tmpdir()), 'twitch-id-'));
  const rom = path.join(dir, 'x.nes');
  fs.writeFileSync(rom, Buffer.concat([header, body]));
  const h = romHashes(fs.readFileSync(rom));
  assert.equal(h.crc32, romHashes(body).crc32);
  const games = [{ file: 'a.json', game: { name: 'A', match: { crc32: ['00000000'] } } },
    { file: 'b.json', game: { name: 'B', match: { crc32: [h.crc32.toLowerCase()] } } }];
  assert.equal(identify({ rom, prg_fnv1a: '1234ABCD' }, games).entry.game.name, 'B');
  assert.equal(identify({ rom: '', prg_fnv1a: '1234abcd' }, [{ file: 'c', game: { name: 'C', match: { prg_fnv1a: ['1234ABCD'] } } }]).entry.game.name, 'C');
  assert.equal(identify({ rom, prg_fnv1a: '' }, games.slice(0, 1)).entry, null);
  fs.rmSync(dir, { recursive: true });
});

// A fake window: RAM for read_ram, a log of every request.
function fakeGame(ram = {}) {
  const sent = [];
  const reply = (cmd, fields) => {
    sent.push([cmd, fields]);
    if (cmd === 'read_ram') return { ok: true, hex: (ram[fields.addr] ?? 0).toString(16).padStart(2, '0') };
    return { ok: true };
  };
  return {
    sent,
    ram,
    connected: true,
    send: async (cmd, fields) => reply(cmd, fields),
    batch: async (reqs) => reqs.map(([c, f]) => reply(c, f)),
  };
}

const profile = {
  cooldown_ms: 1000,
  user_cooldown_ms: 5000,
  conditions: { playing: [{ addr: '0x0770', eq: 1 }] },
  aliases: { invincible: 'star' },
  commands: {
    star: { cooldown_ms: 30000, when: 'playing', banner: '{user} STAR', steps: [{ ram: { addr: '0x079F', set: '0x23' } }] },
    grow: {
      variants: [
        { when: [{ addr: '0x0756', eq: 0 }], steps: [{ ram: { addr: '0x0756', set: 1 } }] },
        { when: [{ addr: '0x0756', eq: 1 }], steps: [{ ram: { addr: '0x0756', set: 2 } }] },
      ],
    },
    pit: { who: 'mod', steps: [{ ram: { addr: '0xB5', set: 2 } }] },
    big: { bits: 100, steps: [{ pad: { buttons: 'a', frames: 10 } }] },
  },
};
const viewer = (user, text, extra = {}) => ({ user, login: user, text, color: '', bits: 0, rewardId: '', broadcaster: false, mod: false, vip: false, sub: false, ...extra });

test('a command runs once its game state holds, then cools down', async () => {
  let t = 0;
  const game = fakeGame({ 0x0770: 0 });
  const e = createEngine(profile, { game, now: () => t });
  assert.equal((await e.handle(viewer('ann', '!star'))).why, 'not now (game state)');
  game.ram[0x0770] = 1;
  t = 10000;
  assert.equal((await e.handle(viewer('ann', '!invincible'))).ran, true);
  assert.deepEqual(game.sent.filter(([c]) => c === 'chat_ram').at(-1)[1], { addr: 0x079f, set: 0x23 });
  assert.equal(game.sent.at(-1)[0], 'chat_banner');
  assert.equal(game.sent.at(-1)[1].text, 'ann STAR');
  t = 12000;
  assert.match((await e.handle(viewer('ann', '!grow'))).why, /^user cooldown/);
  t = 20000;
  assert.match((await e.handle(viewer('bob', '!star'))).why, /^cooldown/);
});

test('variants pick by game state; permissions and bits gate commands', async () => {
  let t = 0;
  const game = fakeGame({ 0x0756: 1 });
  const e = createEngine(profile, { game, now: () => (t += 10000) });
  await e.handle(viewer('ann', '!grow'));
  assert.deepEqual(game.sent.filter(([c]) => c === 'chat_ram').at(-1)[1], { addr: 0x0756, set: 2 });
  assert.equal((await e.handle(viewer('bob', '!pit'))).why, 'not allowed');
  assert.equal((await e.handle(viewer('cat', '!pit', { mod: true }))).ran, true);
  assert.equal((await e.handle(viewer('dan', '!big'))).why, 'needs 100 bits');
  assert.equal(await e.handle(viewer('dan', 'cheer100 !big', { bits: 100 })), null); // the command must lead
  assert.equal((await e.handle(viewer('eve', '!big', { bits: 100 }))).ran, true);
});

test('every message is shown in the chat panel, commands included', async () => {
  const game = fakeGame();
  const e = createEngine(profile, { game, now: () => 0 });
  await e.handle(viewer('ann', 'hello there'));
  await e.handle(viewer('ann', '!commands'));
  const chat = game.sent.filter(([c]) => c === 'chat').map(([, f]) => f.text);
  assert.deepEqual(chat, ['hello there', '!commands', 'COMMANDS: !star !grow !big']);
});

const voteProfile = {
  ...profile,
  vote: { mode: 'vote', options: 3, pool: ['star', 'grow', 'big'], duration_ms: 20000 },
};

test('a vote round: one vote per viewer, the winner runs past its cooldown', async () => {
  let t = 100000;
  const game = fakeGame({ 0x0770: 1, 0x0756: 0 });
  const e = createEngine(voteProfile, { game, now: () => t, random: () => 0 });
  assert.deepEqual(await e.startVote(), ['star', 'grow', 'big']);
  assert.equal((await e.handle(viewer('ann', '2'))).voted, true);
  await e.handle(viewer('bob', '!vote 2'));
  await e.handle(viewer('cat', '1'));
  await e.handle(viewer('cat', '!star'));       // a command on the ballot is a vote too; cat stays on 1
  await e.handle(viewer('ann', '3'));           // ann changes her mind
  await e.handle(viewer('dan', '7'));           // no such option: plain chat
  const poll = game.sent.filter(([c]) => c === 'chat_poll').at(-1)[1];
  assert.equal(poll.options, 'STAR|GROW|BIG');
  assert.deepEqual(game.sent.filter(([c]) => c === 'chat').map(([, f]) => f.text), ['7']); // votes show in the poll only
  const r = await e.finishVote();
  assert.deepEqual(r, { winner: 'star', votes: [1, 1, 1] }); // a three-way tie, broken by random()
  assert.ok(game.sent.some(([c, f]) => c === 'chat_banner' && /^CHAT CHOSE /.test(f.text)));
});

test('the most voted option that can run now wins; vote mode keeps viewers off direct commands', async () => {
  const game = fakeGame({ 0x0770: 0, 0x0756: 0 });      // not playing: !star cannot run
  const e = createEngine(voteProfile, { game, now: () => 0, random: () => 0 });
  await e.startVote();
  await e.handle(viewer('ann', '1'));
  await e.handle(viewer('bob', '1'));
  await e.handle(viewer('cat', '2'));
  const r = await e.finishVote();
  assert.equal(r.winner, 'grow');
  assert.deepEqual(game.sent.filter(([c]) => c === 'chat_ram').at(-1)[1], { addr: 0x0756, set: 1 });
  assert.equal((await e.handle(viewer('eve', '!grow'))).why, 'vote mode: commands win by vote');
  assert.deepEqual(await e.handle(viewer('mo', '!vote off', { mod: true })), { vote: 'off' });
  assert.equal(e.voting, false);
  assert.equal(await e.handle(viewer('eve', '!vote on')), null); // viewers cannot switch it
  assert.equal(e.voting, false);
});

test('no votes: nothing runs', async () => {
  const game = fakeGame({ 0x0770: 1 });
  const e = createEngine(voteProfile, { game, now: () => 0, random: () => 0.5 });
  await e.startVote();
  assert.deepEqual(await e.finishVote(), { winner: null, votes: [0, 0, 0] });
  assert.equal(game.sent.filter(([c]) => c === 'chat_ram').length, 0);
});
