// semantic.mjs - commands written once against what a game's RAM *means*.
// A game file declares vars (lives, health, timer, coins, score, ...) and
// gets every command here whose `requires` it covers; `when: "playing"`
// uses the game file's "playing" condition when it has one.
//
// A var step works in the var's own units: { var: { name, set | add, min,
// max, frames, every, label } }, where set/add/min/max may also be "min",
// "max", "step", "-step" or "min+1" (the var's own bounds and step).

const v = (name, op) => ({ var: { name, ...op } });

export const semantic = {
  aliases: { '1up': 'oneup', refill: 'heal', drain: 'poison', money: 'rich', points: 'score' },
  commands: {
    oneup: { requires: ['lives'], title: '1-UP', help: 'an extra life', cooldown_ms: 120000, when: 'playing',
      banner: '{user}: 1-UP!', steps: [v('lives', { add: 1 })] },
    heal: { requires: ['health'], title: 'FULL HEAL', help: 'full health', cooldown_ms: 60000, when: 'playing',
      banner: '{user} HEALED YOU!', steps: [v('health', { set: 'max' })] },
    hurt: { requires: ['health'], title: 'OUCH', help: 'some damage, never the last point', cooldown_ms: 30000, when: 'playing',
      banner: '{user} ZAPPED YOU!', steps: [v('health', { add: '-step', min: 'min+1' })] },
    poison: { requires: ['health'], title: 'POISON', help: '10 s of -1 health every half second, never the last point',
      cooldown_ms: 90000, when: 'playing',
      steps: [v('health', { add: -1, min: 'min+1', frames: 600, every: 30, label: 'POISON' })] },
    regen: { requires: ['health'], title: 'REGEN', help: '10 s of +1 health every third of a second', cooldown_ms: 90000,
      when: 'playing', steps: [v('health', { add: 1, frames: 600, every: 20, label: 'REGEN' })] },
    time: { requires: ['timer'], title: '+TIME', help: 'more time on the clock', cooldown_ms: 60000, when: 'playing',
      banner: '{user}: MORE TIME!', steps: [v('timer', { add: 'step' })] },
    hurry: { requires: ['timer'], title: '-TIME', help: 'less time on the clock (never runs out)', cooldown_ms: 90000,
      when: 'playing', banner: '{user}: HURRY UP!', steps: [v('timer', { add: '-step', min: 'step' })] },
    rich: { requires: ['coins'], title: 'COINS', help: 'a handful of coins', cooldown_ms: 60000, when: 'playing',
      banner: '{user}: KA-CHING!', steps: [v('coins', { add: 'step' })] },
    score: { requires: ['score'], title: 'POINTS', help: 'bonus points', cooldown_ms: 30000, when: 'playing',
      banner: '{user}: BONUS POINTS!', steps: [v('score', { add: 'step' })] },
  },
};

// The default step of each well-known var, when its game file gives none.
export const defaultSteps = { health: 4, timer: 100, coins: 10, score: 1000, lives: 1 };
