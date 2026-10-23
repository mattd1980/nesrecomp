// universal.mjs - commands every game gets, with no game file: they act on
// the console (input, picture, clock, sound, save states), never on a game's
// RAM. A game file can override any of them by name, or switch them off with
// "disable".

const fx = (effect, ms, extra = {}) => ({ fx: { effect, ms, ...extra } });
const pad = (buttons, mode, frames, extra = {}) => ({ pad: { buttons, mode, frames, ...extra } });

export const universal = {
  aliases: { invert: 'reverse', mirror: 'flip', upside: 'upsidedown', slow: 'slowmo', grey: 'gray', dark: 'lightsout' },
  commands: {
    // ---- input ----
    jump: { title: 'JUMP', help: 'press A', cooldown_ms: 3000, vote: false, steps: [pad('a', 'press', 20)] },
    left: { title: 'PUSH LEFT', help: 'hold Left a moment', cooldown_ms: 3000, vote: false, steps: [pad('left', 'press', 40)] },
    right: { title: 'PUSH RIGHT', help: 'hold Right a moment', cooldown_ms: 3000, vote: false, steps: [pad('right', 'press', 40)] },
    hop: { title: 'BUNNY HOP', help: '5 s of A tapped', cooldown_ms: 30000, steps: [pad('a', 'tap', 300, { label: 'BUNNY HOP' })] },
    reverse: { title: 'REVERSED', help: '10 s of Left and Right swapped', cooldown_ms: 60000,
      steps: [pad('left+right', 'swap', 600, { label: 'REVERSED' })] },
    swapab: { title: 'A/B SWAPPED', help: '10 s of A and B swapped', cooldown_ms: 60000,
      steps: [pad('a+b', 'swap', 600, { label: 'A/B SWAPPED' })] },
    lag: { title: 'LAG', help: '10 s of every button a quarter second late', cooldown_ms: 60000,
      steps: [pad('a+b+select+start+up+down+left+right', 'lag', 600, { delay: 15, label: 'LAG' })] },
    drunk: { title: 'DRUNK', help: '8 s of the D-pad lurching on its own', cooldown_ms: 60000,
      steps: [pad('up+down+left+right', 'drunk', 480, { delay: 10, label: 'DRUNK' })] },
    nojump: { title: 'NO A BUTTON', help: '5 s without A', cooldown_ms: 60000, steps: [pad('a', 'block', 300, { label: 'NO A BUTTON' })] },
    stun: { title: 'STUNNED', help: '1.5 s with no input at all', cooldown_ms: 45000,
      steps: [pad('a+b+up+down+left+right', 'block', 90, { label: 'STUNNED' })] },

    // ---- picture ----
    flip: { title: 'MIRROR WORLD', help: '15 s flipped left to right', cooldown_ms: 60000,
      steps: [fx('flip_h', 15000, { label: 'MIRROR WORLD' })] },
    upsidedown: { title: 'UPSIDE DOWN', help: '10 s upside down', cooldown_ms: 60000,
      steps: [fx('flip_v', 10000, { label: 'UPSIDE DOWN' })] },
    gray: { title: 'GRAYSCALE', help: '20 s in black and white', cooldown_ms: 30000, steps: [fx('gray', 20000, { label: 'GRAYSCALE' })] },
    negative: { title: 'NEGATIVE', help: '10 s of inverted colours', cooldown_ms: 45000, steps: [fx('negative', 10000, { label: 'NEGATIVE' })] },
    rainbow: { title: 'RAINBOW', help: '15 s of cycling colours', cooldown_ms: 30000, steps: [fx('rainbow', 15000, { label: 'RAINBOW' })] },
    redalert: { title: 'RED ALERT', help: '15 s tinted red', cooldown_ms: 30000,
      steps: [fx('tint', 15000, { color: 'FF2020', label: 'RED ALERT' })] },
    pixel: { title: 'PIXELATED', help: '15 s of big pixels', cooldown_ms: 45000, steps: [fx('pixelate', 15000, { amount: 4, label: 'PIXELATED' })] },
    shake: { title: 'EARTHQUAKE', help: '4 s of shaking', cooldown_ms: 20000, steps: [fx('shake', 4000, { amount: 3, label: 'EARTHQUAKE' })] },
    zoom: { title: 'ZOOM', help: '10 s zoomed in', cooldown_ms: 45000, steps: [fx('zoom', 10000, { amount: 160, follow: 'player', label: 'ZOOM' })] },
    wobble: { title: 'DIZZY', help: '10 s of a wobbling picture', cooldown_ms: 45000, steps: [fx('wobble', 10000, { amount: 6, label: 'DIZZY' })] },
    lightsout: { title: 'LIGHTS OUT', help: '15 s of darkness but for a light on the player (the middle, if the game file has no player_screen)', cooldown_ms: 60000,
      steps: [fx('spotlight', 15000, { amount: 30, follow: 'player', label: 'LIGHTS OUT' })] },
    twin: { title: 'TWIN VIEW', help: '15 s of the left half mirrored', cooldown_ms: 60000, steps: [fx('mirror', 15000, { label: 'TWIN VIEW' })] },

    // ---- clock ----
    slowmo: { title: 'SLOW MOTION', help: '8 s at half speed', cooldown_ms: 45000,
      steps: [{ time: { mode: 'speed', percent: 50, ms: 8000, label: 'SLOW MOTION' } }] },
    fast: { title: 'FAST FORWARD', help: '8 s at one and a half speed', cooldown_ms: 45000,
      steps: [{ time: { mode: 'speed', percent: 150, ms: 8000, label: 'FAST FORWARD' } }] },
    timestop: { title: 'TIME STOP', help: 'the game stops for 2 s', cooldown_ms: 60000,
      steps: [{ time: { mode: 'stop', ms: 2000, label: 'TIME STOP' } }] },
    rewind: { title: 'REWIND', help: 'back 5 seconds', cooldown_ms: 60000, banner: '{user} REWOUND TIME!',
      steps: [{ rewind: { seconds: 5 } }] },
    save: { title: 'CHECKPOINT', help: 'moderators: a checkpoint chat can return to', who: 'mod', cooldown_ms: 5000,
      banner: 'CHECKPOINT SAVED', steps: [{ checkpoint: 'save' }] },
    load: { title: 'BACK TO CHECKPOINT', help: 'back to the checkpoint', cooldown_ms: 180000, vote: false,
      banner: '{user}: BACK TO THE CHECKPOINT!', steps: [{ checkpoint: 'load' }] },

    // ---- sound ----
    silence: { title: 'SILENCE', help: '10 s of no sound', cooldown_ms: 60000, steps: [{ audio: { mute: 63, ms: 10000, label: 'SILENCE' } }] },
    nobass: { title: 'NO BASS', help: '15 s without the triangle channel', cooldown_ms: 60000,
      steps: [{ audio: { mute: 4, ms: 15000, label: 'NO BASS' } }] },

    // ---- chaos ----
    glitch: { title: 'GLITCH', help: 'one random RAM byte scrambled (!rewind undoes it)', cooldown_ms: 120000, vote: false,
      banner: '{user} GLITCHED THE GAME!', steps: [{ glitch: {} }] },
  },
};
