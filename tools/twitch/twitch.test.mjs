// node --test tools/twitch/
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { buttons, compileProfile, createEngine, nameColor, printable } from './engine.mjs';
import { chatMessage, parseIrcLine } from './irc.mjs';

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

test('every bundled profile compiles', () => {
  for (const f of fs.readdirSync(path.join(here, 'profiles'))) {
    const p = compileProfile(JSON.parse(fs.readFileSync(path.join(here, 'profiles', f), 'utf8')));
    assert.ok(p.commands.size > 0, f);
  }
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
