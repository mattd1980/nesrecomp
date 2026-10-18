// engine.mjs - what a chat message does to the game, from a game profile
// (profiles/*.json, described in README.md): every message goes to the chat
// panel, and a !command that passes its permission, cooldown and game-state
// checks runs its steps through the window's chat_* requests (cyc_chat.h).

const BUTTONS = { a: 0x80, b: 0x40, select: 0x20, start: 0x10, up: 0x08, down: 0x04, left: 0x02, right: 0x01 };
const NAME_COLORS = ['FF4A4A', '4A8CFF', '3CC83C', 'FFB43C', 'C864FF', '3CD2D2', 'FF6EB4', 'B4DC3C'];

// "0x79F", "$079F", 1951 -> 1951
export function num(v, what = 'number') {
  if (typeof v === 'number' && Number.isInteger(v)) return v;
  if (typeof v === 'string') {
    const s = v.trim();
    const n = /^\$[0-9a-f]+$/i.test(s) ? parseInt(s.slice(1), 16) : Number(s);
    if (Number.isInteger(n)) return n;
  }
  throw new Error(`not a ${what}: ${JSON.stringify(v)}`);
}

// "a+b", ["left","a"], 0x82 -> NES button bits
export function buttons(v) {
  if (typeof v === 'number') return v;
  const names = Array.isArray(v) ? v : String(v).split(/[+, ]+/).filter(Boolean);
  let bits = 0;
  for (const n of names) {
    const b = BUTTONS[n.toLowerCase()];
    if (b === undefined) throw new Error(`unknown button ${n}`);
    bits |= b;
  }
  return bits;
}

// Chat text the in-game font can draw: printable ASCII, accents dropped.
export function printable(s, max = 200) {
  const ascii = s
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/[^\x20-\x7e]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  return ascii.length > max ? ascii.slice(0, max - 3) + '...' : ascii;
}

export function nameColor(msg) {
  const c = /^#?([0-9a-f]{6})$/i.exec(msg.color ?? '');
  if (c) return c[1].toUpperCase();
  let h = 0;
  for (const ch of msg.login ?? msg.user) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  return NAME_COLORS[h % NAME_COLORS.length];
}

// One step of a command -> [request, fields] for the window, or a wait.
function compileStep(step, where) {
  const keys = Object.keys(step);
  if (keys.length !== 1) throw new Error(`${where}: a step has exactly one of ram, pad, call, banner, wait_ms`);
  const [kind] = keys;
  const s = step[kind];
  switch (kind) {
    case 'ram': {
      const f = { addr: num(s.addr, 'address') };
      const ops = ['set', 'add', 'or', 'and'].filter((k) => k in s);
      if (ops.length !== 1) throw new Error(`${where}: ram needs one of set, add, or, and`);
      f[ops[0]] = num(s[ops[0]]);
      for (const k of ['min', 'max', 'frames', 'every', 'if_addr', 'if_eq']) if (k in s) f[k] = num(s[k]);
      if (s.label) f.label = printable(s.label, 38);
      return ['chat_ram', f];
    }
    case 'pad': {
      const f = { buttons: buttons(s.buttons), mode: s.mode ?? 'press', frames: num(s.frames ?? 30) };
      if (!['press', 'tap', 'block', 'invert'].includes(f.mode)) throw new Error(`${where}: pad mode ${f.mode}`);
      if (s.label) f.label = printable(s.label, 38);
      return ['chat_pad', f];
    }
    case 'call': {
      const f = { addr: num(s.addr, 'address') };
      for (const k of ['a', 'x', 'y']) if (k in s) f[k] = num(s[k]);
      return ['chat_call', f];
    }
    case 'banner':
      return ['chat_banner', typeof s === 'string' ? { text: s } : { ...s }];
    case 'wait_ms':
      return ['wait', { ms: num(s) }];
    default:
      throw new Error(`${where}: unknown step ${kind}`);
  }
}

// [{addr, eq|ne|min|max, mask}] or the name of a profile condition.
function compileWhen(when, conditions, where) {
  if (when === undefined) return [];
  if (typeof when === 'string') {
    if (!conditions[when]) throw new Error(`${where}: no condition named ${when}`);
    return conditions[when];
  }
  return when.map((c) => {
    const out = { addr: num(c.addr, 'address'), mask: c.mask === undefined ? 0xff : num(c.mask) };
    for (const k of ['eq', 'ne', 'min', 'max']) if (k in c) out[k] = num(c[k]);
    return out;
  });
}

export function compileProfile(profile) {
  const conditions = {};
  for (const [name, list] of Object.entries(profile.conditions ?? {})) conditions[name] = compileWhen(list, {}, name);
  const commands = new Map();
  for (const [name, c] of Object.entries(profile.commands ?? {})) {
    const where = `!${name}`;
    const variants = (c.variants ?? [c]).map((v, i) => ({
      when: compileWhen(v.when ?? c.when, conditions, `${where} variant ${i}`),
      steps: (v.steps ?? []).map((s) => compileStep(s, where)),
      banner: v.banner ?? c.banner,
    }));
    commands.set(name.toLowerCase(), {
      name,
      help: c.help ?? '',
      who: c.who ?? 'everyone',
      bits: c.bits ?? 0,
      cooldownMs: c.cooldown_ms ?? profile.command_cooldown_ms ?? 10000,
      variants,
      lastUsed: -Infinity,
    });
  }
  const aliases = new Map(Object.entries(profile.aliases ?? {}).map(([a, n]) => [a.toLowerCase(), n.toLowerCase()]));
  for (const [a, n] of aliases) if (!commands.has(n)) throw new Error(`alias !${a}: no command !${n}`);
  const rewards = new Map(Object.entries(profile.rewards ?? {}).map(([id, n]) => [id, n.toLowerCase()]));
  return {
    name: profile.name ?? 'game',
    prefix: profile.prefix ?? '!',
    chat: profile.chat ?? {},
    globalCooldownMs: profile.cooldown_ms ?? 1500,
    userCooldownMs: profile.user_cooldown_ms ?? 8000,
    commands,
    aliases,
    rewards,
  };
}

function allowed(cmd, msg) {
  if (msg.broadcaster) return true;
  switch (cmd.who) {
    case 'broadcaster': return false;
    case 'mod': return msg.mod;
    case 'vip': return msg.mod || msg.vip;
    case 'sub': return msg.mod || msg.vip || msg.sub;
    default: return true;
  }
}

function holds(cond, byte) {
  const v = byte & cond.mask;
  return (cond.eq === undefined || v === cond.eq) && (cond.ne === undefined || v !== cond.ne) &&
    (cond.min === undefined || v >= cond.min) && (cond.max === undefined || v <= cond.max);
}

function fill(template, msg, cmd) {
  return printable(String(template).replaceAll('{user}', msg.user).replaceAll('{command}', cmd.name), 60);
}

// The engine. game.send(cmd, fields) talks to the window; now() is a clock in ms.
export function createEngine(profile, { game, now = () => Date.now(), log = () => {}, sleep } = {}) {
  const p = compileProfile(profile);
  const wait = sleep ?? ((ms) => new Promise((r) => setTimeout(r, ms)));
  const users = new Map();
  let lastEffect = -Infinity;
  let queue = Promise.resolve();

  async function readByte(addr) {
    const r = await game.send('read_ram', { addr, len: 1 });
    return parseInt(r.hex, 16);
  }

  async function variantFor(cmd) {
    for (const v of cmd.variants) {
      let ok = true;
      for (const c of v.when) if (!holds(c, await readByte(c.addr))) { ok = false; break; }
      if (ok) return v;
    }
    return null;
  }

  async function run(cmd, msg) {
    const t = now();
    if (!allowed(cmd, msg)) return { ran: false, why: 'not allowed' };
    if (cmd.bits && msg.bits < cmd.bits && !msg.reward) return { ran: false, why: `needs ${cmd.bits} bits` };
    const privileged = msg.broadcaster || msg.mod;
    if (!privileged && !msg.reward && !msg.bits) {
      if (t - lastEffect < p.globalCooldownMs) return { ran: false, why: 'global cooldown' };
      const u = users.get(msg.login) ?? -Infinity;
      if (t - u < p.userCooldownMs) return { ran: false, why: `user cooldown ${Math.ceil((p.userCooldownMs - (t - u)) / 1000)}s` };
      if (t - cmd.lastUsed < cmd.cooldownMs) return { ran: false, why: `cooldown ${Math.ceil((cmd.cooldownMs - (t - cmd.lastUsed)) / 1000)}s` };
    }
    const v = await variantFor(cmd);
    if (!v) return { ran: false, why: 'not now (game state)' };
    lastEffect = cmd.lastUsed = t;
    users.set(msg.login, t);
    // steps between waits go as one batch: one gap between frames, so a state
    // change made of several stores is never seen half done
    let batch = [];
    const flush = async () => {
      if (batch.length) await game.batch(batch);
      batch = [];
    };
    for (const [req, fields] of v.steps) {
      if (req === 'wait') {
        await flush();
        await wait(fields.ms);
      } else {
        batch.push([req, req === 'chat_banner' ? { ...fields, text: fill(fields.text, msg, cmd) } : fields]);
      }
    }
    if (v.banner) batch.push(['chat_banner', { text: fill(v.banner, msg, cmd), color: 'FFD25A', ms: 2500 }]);
    await flush();
    return { ran: true };
  }

  function lookup(text) {
    if (!text.startsWith(p.prefix)) return null;
    const word = text.slice(p.prefix.length).split(/\s+/)[0].toLowerCase();
    const name = p.aliases.get(word) ?? word;
    return { word, cmd: p.commands.get(name) };
  }

  function helpLine() {
    return 'COMMANDS: ' + [...p.commands.values()].filter((c) => c.who === 'everyone').map((c) => p.prefix + c.name).join(' ');
  }

  // One chat message: shown, then (serialised with the others) acted on.
  function handle(msg) {
    const text = printable(msg.text);
    const rewardCmd = msg.rewardId ? p.rewards.get(msg.rewardId) : undefined;
    if (msg.rewardId && !rewardCmd) log(`unmapped channel-point reward id ${msg.rewardId} (add it to "rewards")`);
    const job = queue.then(async () => {
      if (text) {
        await game.send('chat', { user: printable(msg.user, 24), color: nameColor(msg), text }).catch((e) => log(`chat: ${e.message}`));
      }
      const hit = rewardCmd ? { word: rewardCmd, cmd: p.commands.get(rewardCmd) } : lookup(text);
      if (!hit) return null;
      if (hit.word === 'commands' || hit.word === 'help') {
        if (!hit.cmd) await game.send('chat', { user: '', color: 'FFD25A', text: helpLine() }).catch(() => {});
        return { command: hit.word, ran: !hit.cmd };
      }
      if (!hit.cmd) return null;
      try {
        const r = await run(hit.cmd, { ...msg, reward: !!rewardCmd });
        log(`${msg.user} !${hit.cmd.name}: ${r.ran ? 'ran' : r.why}`);
        return { command: hit.cmd.name, ...r };
      } catch (e) {
        log(`${msg.user} !${hit.cmd.name}: failed: ${e.message}`);
        return { command: hit.cmd.name, ran: false, why: e.message };
      }
    });
    queue = job.catch(() => {});
    return job;
  }

  async function configure() {
    const c = p.chat;
    const fields = {};
    for (const k of ['show', 'side', 'valign', 'offset', 'lines', 'ms', 'scale']) if (k in c) fields[k] = c[k];
    if (Object.keys(fields).length) await game.send('chat_config', fields);
  }

  return { handle, configure, helpLine, profile: p };
}
