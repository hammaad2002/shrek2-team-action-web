/**
 * Game settings: what the original Launcher (Game/Launcher.exe) let players change, stored in the
 * registry values the game reads at start-up (loader at 0x593cf0 and WinMain).
 *
 * - `k<p>_<a>`: DirectInput key code for player p, action a; `gp<p>_<a>`: gamepad binding code
 *   (1 lX, 2 lY, 3 lZ, 4/5 POV left-right/up-down, 6 lRx, 7 lRy, 11+n button n); `pad<p>`: the
 *   GUID of the pad player p uses — here always virtual pad p (runtime/src/hle/dinput8.cpp).
 * - A stick bound to the first action of a pair (Move Up, Move Left, Zoom In, Cam Left) drives
 *   both directions; bound to the second action it is inverted (the game's XInput emulation at
 *   0x594320 combines each pair into one stick axis). The Launcher's defaults bind it to both.
 * - `options` bits the game reads: 0x2 detailed shadows, 0x20 anti-aliasing, 0x100 distance
 *   clipping (with `distance` 0..100). Missing values read as 0xFFFFFFFF, so defaults are written
 *   before the first start.
 */
import { KEYS } from "./keys.ts";
import { Registry, SETTINGS_KEY } from "./registry.ts";

export const PLAYERS = 4;

/** The game's 16 configurable actions, in registry order, with their action IDs. */
export const ACTIONS: { name: string; id: number }[] = [
  { name: "Move Up", id: 0x10 },
  { name: "Move Down", id: 0x11 },
  { name: "Move Left", id: 0x12 },
  { name: "Move Right", id: 0x13 },
  { name: "Zoom In", id: 0x14 },
  { name: "Zoom Out", id: 0x15 },
  { name: "Cam Left", id: 0x16 },
  { name: "Cam Right", id: 0x17 },
  { name: "Jump", id: 0x4 },
  { name: "Use", id: 0x5 },
  { name: "Attack", id: 0x6 },
  { name: "Special", id: 0x7 },
  { name: "Start (pause)", id: 0xc },
  { name: "Next Character", id: 0xb },
  { name: "Previous Character", id: 0xa },
  { name: "Display HUD", id: 0xd },
];

/** Action pairs that share one stick axis: [first, second]. */
export const AXIS_PAIRS: [number, number][] = [
  [0, 1],
  [2, 3],
  [4, 5],
  [6, 7],
];

export const RESOLUTIONS = ["640 × 480", "800 × 600", "1024 × 768", "1280 × 1024"];

export const OPT_SHADOWS = 0x2;
export const OPT_CLIPPING = 0x100;

export interface GameSettings {
  resolution: number;
  options: number;
  distance: number;
  mouseEnabled: boolean;
  mouseSensitivity: number;
  mouseWheel: boolean;
  /** Action IDs for the left, right and middle mouse buttons (0: none). */
  mouseButtons: [number, number, number];
  /** [player][action] DirectInput key code (0: none). */
  keys: number[][];
  /** [player][action] gamepad binding code (0: none). */
  pads: number[][];
}

// The Launcher's defaults (its tables at 0x42b668 and its GetProfileInt defaults); the keys match
// the controls printed in the manual. Its gamepad defaults (player 1 only, for buttons 7-10 of a
// 2004 pad) are replaced by a layout for today's Xbox-style controllers, for every player.
const DEFAULT_KEYS = [0x11, 0x1f, 0x1e, 0x20, 0xc8, 0xd0, 0xcb, 0xcd, 0x39, 0x21, 0x1c, 0x2a, 0x01, 0x4f, 0x50, 0x0f];
const DEFAULT_PAD = [
  2, 2, // left stick up/down
  1, 1, // left stick left/right
  7, 7, // right stick up/down: zoom
  6, 6, // right stick left/right: camera
  11, // A: Jump
  12, // B: Use
  13, // X: Attack
  14, // Y: Special
  18, // Start
  16, // RB: Next Character
  15, // LB: Previous Character
  17, // Back: Display HUD
];

export function defaultSettings(): GameSettings {
  return {
    resolution: 2,
    options: 0x22,
    distance: 100,
    mouseEnabled: true,
    mouseSensitivity: 100,
    mouseWheel: true,
    mouseButtons: [0x6, 0x4, 0x5],
    keys: Array.from({ length: PLAYERS }, (_, p) => (p === 0 ? [...DEFAULT_KEYS] : Array(ACTIONS.length).fill(0))),
    pads: Array.from({ length: PLAYERS }, () => [...DEFAULT_PAD]),
  };
}

/** Virtual pad p's instance GUID as the game's parser (0x5935d0) expects it: upper-case, no braces. */
export function padGuid(p: number): string {
  return `53324750-4144-0001-8000-00000000000${p}`;
}

export function hasSettings(r: Registry): boolean {
  return r.dword(SETTINGS_KEY, "resolution") !== undefined;
}

export function readSettings(r: Registry): GameSettings {
  const d = defaultSettings();
  const dw = (name: string, fallback: number) => r.dword(SETTINGS_KEY, name) ?? fallback;
  return {
    resolution: Math.min(3, dw("resolution", d.resolution)),
    options: dw("options", d.options),
    distance: Math.min(100, dw("distance", d.distance)),
    mouseEnabled: dw("mouseenable", 1) !== 0,
    mouseSensitivity: Math.min(100, dw("mousesens", d.mouseSensitivity)),
    mouseWheel: dw("mousewheel", 1) !== 0,
    mouseButtons: [dw("mousebindl", d.mouseButtons[0]), dw("mousebindr", d.mouseButtons[1]), dw("mousebindm", d.mouseButtons[2])],
    keys: d.keys.map((row, p) => row.map((v, a) => dw(`k${p}_${a}`, v))),
    pads: d.pads.map((row, p) => row.map((v, a) => dw(`gp${p}_${a}`, v))),
  };
}

export function writeSettings(s: GameSettings, r: Registry): void {
  const dw = (name: string, value: number) => r.set(SETTINGS_KEY, name, { type: "dword", value });
  dw("resolution", s.resolution);
  dw("options", s.options);
  dw("distance", s.distance);
  dw("mouseenable", s.mouseEnabled ? 1 : 0);
  dw("mousesens", s.mouseSensitivity);
  dw("mousewheel", s.mouseWheel ? 1 : 0);
  dw("mousebindl", s.mouseButtons[0]);
  dw("mousebindr", s.mouseButtons[1]);
  dw("mousebindm", s.mouseButtons[2]);
  for (let p = 0; p < PLAYERS; p++) {
    for (let a = 0; a < ACTIONS.length; a++) {
      dw(`k${p}_${a}`, s.keys[p][a]);
      dw(`gp${p}_${a}`, s.pads[p][a]);
    }
    r.set(SETTINGS_KEY, `pad${p}`, { type: "sz", value: padGuid(p) });
  }
}

// ---- Labels ------------------------------------------------------------------------------------

/** DirectInput key code (as stored in k<p>_<a>) for a KeyboardEvent.code. */
export function dikForCode(code: string): number | undefined {
  const k = KEYS[code];
  if (!k) return undefined;
  const scan = k[1];
  return scan & 0x100 ? 0x80 | (scan & 0x7f) : scan;
}

const CODE_FOR_DIK = new Map<number, string>();
for (const code of Object.keys(KEYS)) {
  const dik = dikForCode(code)!;
  if (!CODE_FOR_DIK.has(dik)) CODE_FOR_DIK.set(dik, code);
}

export function keyLabel(code: string): string {
  const named: Record<string, string> = {
    ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→", Escape: "Esc", Space: "Space", Enter: "Enter",
    NumpadEnter: "Num Enter", ShiftLeft: "Left Shift", ShiftRight: "Right Shift", ControlLeft: "Left Ctrl",
    ControlRight: "Right Ctrl", AltLeft: "Left Alt", AltRight: "Right Alt", Backquote: "`", Minus: "-", Equal: "=",
    BracketLeft: "[", BracketRight: "]", Backslash: "\\", Semicolon: ";", Quote: "'", Comma: ",", Period: ".", Slash: "/",
    NumpadMultiply: "Num *", NumpadSubtract: "Num -", NumpadAdd: "Num +", NumpadDecimal: "Num .", NumpadDivide: "Num /",
    PageUp: "Page Up", PageDown: "Page Down", CapsLock: "Caps Lock", NumLock: "Num Lock", ScrollLock: "Scroll Lock",
  };
  if (named[code]) return named[code];
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit\d$/.test(code)) return code.slice(5);
  if (/^Numpad\d$/.test(code)) return `Num ${code.slice(6)}`;
  return code;
}

export function dikLabel(dik: number): string {
  if (!dik) return "—";
  const code = CODE_FOR_DIK.get(dik);
  return code ? keyLabel(code) : `Key ${dik}`;
}

const PAD_BUTTONS = ["A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Left stick press", "Right stick press", "LT", "RT"];
const PAD_AXES: Record<number, string> = {
  1: "Left stick ↔",
  2: "Left stick ↕",
  3: "Triggers",
  4: "D-pad ↔",
  5: "D-pad ↕",
  6: "Right stick ↔",
  7: "Right stick ↕",
};

export function isAxisCode(code: number): boolean {
  return code >= 1 && code <= 10;
}

export function padLabel(code: number): string {
  if (!code) return "—";
  if (isAxisCode(code)) return PAD_AXES[code] ?? `Axis ${code}`;
  const n = code - 11;
  return PAD_BUTTONS[n] ?? `Button ${n + 1}`;
}

/** Gamepad binding code for a standard-mapping button index (D-pad buttons give POV codes). */
export function padCodeForButton(index: number): { code: number; sign: number } | undefined {
  const di = [0, 1, 2, 3, 4, 5, 10, 11, 6, 7, 8, 9]; // standard index → DirectInput button
  if (index < 12) return { code: 11 + di[index], sign: 0 };
  if (index === 12 || index === 13) return { code: 5, sign: index === 12 ? -1 : 1 };
  if (index === 14 || index === 15) return { code: 4, sign: index === 14 ? -1 : 1 };
  return undefined;
}

/** Gamepad binding code for a standard-mapping axis index. */
export function padCodeForAxis(index: number): number | undefined {
  return [1, 2, 6, 7][index];
}

/**
 * Bind gamepad `code` to `action` for one player. For an axis (sign: the direction it was moved,
 * -1 up/left, +1 down/right) on a paired action, the axis goes to the pair's first action when
 * that direction matches the action, otherwise to the second action (inverted).
 */
export function bindPad(row: number[], action: number, code: number, sign = 0): void {
  const pair = AXIS_PAIRS.find((p) => p.includes(action));
  const clearElsewhere = (keep: number[]) => {
    for (let a = 0; a < row.length; a++) if (!keep.includes(a) && row[a] === code) row[a] = 0;
  };
  if (pair && isAxisCode(code) && sign !== 0) {
    const [first, second] = pair;
    const natural = (action === first && sign < 0) || (action === second && sign > 0);
    clearElsewhere(pair);
    if (natural) {
      row[first] = code;
      row[second] = code;
    } else {
      row[first] = 0;
      row[second] = code;
    }
    return;
  }
  // A paired action losing its stick takes the stick off its partner too (else the partner's copy
  // would become an inverted binding).
  if (pair) {
    const partner = pair[0] === action ? pair[1] : pair[0];
    if (isAxisCode(row[action]) && row[partner] === row[action]) row[partner] = 0;
  }
  clearElsewhere([action]);
  row[action] = code;
}

/** Label for a player's binding of `action`, noting inverted sticks. */
export function padBindingLabel(row: number[], action: number): string {
  const code = row[action];
  const pair = AXIS_PAIRS.find((p) => p.includes(action));
  if (pair && isAxisCode(code) && action === pair[1] && row[pair[0]] !== code) return `${padLabel(code)} (inverted)`;
  return padLabel(code);
}

/** Bind a key to an action for one player; the key leaves any other action of that player. */
export function bindKey(row: number[], action: number, dik: number): void {
  for (let a = 0; a < row.length; a++) if (row[a] === dik) row[a] = 0;
  row[action] = dik;
}
