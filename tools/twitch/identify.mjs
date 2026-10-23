// identify.mjs - which game is running, and its game file.
//
// The window answers rom_info with the ROM file it opened and the FNV-1a
// hash of its PRG ROM. The bridge hashes that file the way No-Intro lists
// cartridges (CRC32 and SHA-1 of the ROM without its iNES header or trainer)
// and picks the game file whose "match" names one of those, or the PRG hash.

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

const CRC_TABLE = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

export function crc32(bytes) {
  let c = ~0;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 255] ^ (c >>> 8);
  return ((~c) >>> 0).toString(16).toUpperCase().padStart(8, '0');
}

// Hashes of a ROM image. An iNES/NES 2.0 file loses its 16-byte header (and a
// 512-byte trainer when flag 6 bit 2 says one follows); anything else is
// hashed whole (an FDS image, a headerless dump).
export function romHashes(bytes) {
  const ines = bytes.length >= 16 && bytes[0] === 0x4e && bytes[1] === 0x45 && bytes[2] === 0x53 && bytes[3] === 0x1a;
  const body = ines ? bytes.subarray(16 + (bytes[6] & 4 ? 512 : 0)) : bytes;
  return {
    crc32: crc32(body),
    sha1: crypto.createHash('sha1').update(body).digest('hex'),
    file_sha1: crypto.createHash('sha1').update(bytes).digest('hex'),
  };
}

// Every game file in these directories: [{ file, game }].
export function loadGames(dirs) {
  const out = [];
  for (const dir of dirs) {
    if (!fs.existsSync(dir)) continue;
    for (const f of fs.readdirSync(dir).filter((n) => n.endsWith('.json')).sort()) {
      const file = path.join(dir, f);
      out.push({ file, game: JSON.parse(fs.readFileSync(file, 'utf8')) });
    }
  }
  return out;
}

const same = (a, b) => String(a).toLowerCase() === String(b).toLowerCase();

// The game file for a running ROM: { identity, entry } (entry null when no
// game file names it). identity holds what was learned, for the log.
export function identify(romInfo, games) {
  const identity = { rom: romInfo.rom ?? '', prg_fnv1a: romInfo.prg_fnv1a ?? '' };
  try {
    if (identity.rom) Object.assign(identity, romHashes(fs.readFileSync(identity.rom)));
  } catch (e) {
    identity.error = `cannot read ${identity.rom}: ${e.message}`;
  }
  const entry = games.find(({ game }) => {
    const m = game.match ?? {};
    return (m.crc32 ?? []).some((h) => identity.crc32 && same(h, identity.crc32)) ||
      (m.sha1 ?? []).some((h) => identity.sha1 && same(h, identity.sha1)) ||
      (m.prg_fnv1a ?? []).some((h) => identity.prg_fnv1a && same(h, identity.prg_fnv1a));
  });
  return { identity, entry: entry ?? null };
}
