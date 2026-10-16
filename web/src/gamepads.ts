/**
 * Controllers → the engine's four virtual gamepads, one per player. A controller takes the first
 * free player slot when the browser first reports it (most browsers do so after a button press)
 * and keeps it until it disconnects; the game sees an empty slot as an unplugged pad.
 */
export interface PadEngine {
  HEAPU8: Uint8Array;
  _web_gamepad(slot: number, connected: number, buttons: number, lx: number, ly: number, rx: number, ry: number, lt: number, rt: number): void;
  _web_gamepad_rumble(): number;
}

export const PLAYER_SLOTS = 4;

type Haptics = { playEffect(type: string, params: object): Promise<unknown>; reset?(): Promise<unknown> };

export function connectedPads(): Gamepad[] {
  try {
    return [...(navigator.getGamepads?.() ?? [])].filter((g): g is Gamepad => !!g && g.connected);
  } catch {
    return []; // e.g. blocked by a permissions policy
  }
}

/** A short, readable controller name ("Xbox Wireless Controller (STANDARD GAMEPAD …)" → "Xbox Wireless Controller"). */
export function padName(gp: Gamepad): string {
  const name = gp.id.replace(/\s*\((?:STANDARD GAMEPAD|Vendor:|[0-9a-f]{4}-[0-9a-f]{4}).*$/i, "").replace(/^[0-9a-f]{4}-[0-9a-f]{4}-/i, "").trim();
  return name || "Controller";
}

/** Buttons in the W3C standard order as a bit mask (the engine's PadButton bits), plus stick and trigger values. */
export function readPad(gp: Gamepad) {
  let buttons = 0;
  for (let i = 0; i < 16 && i < gp.buttons.length; i++) if (gp.buttons[i].pressed) buttons |= 1 << i;
  const axis = (i: number) => (i < gp.axes.length && Number.isFinite(gp.axes[i]) ? gp.axes[i] : 0);
  const trigger = (i: number) => (i < gp.buttons.length ? gp.buttons[i].value : 0);
  return { buttons, lx: axis(0), ly: axis(1), rx: axis(2), ry: axis(3), lt: trigger(6), rt: trigger(7) };
}

export class Gamepads {
  /** Gamepad.index of the controller in each player slot (null: none). */
  readonly slots: (number | null)[] = Array(PLAYER_SLOTS).fill(null);
  private readonly played = Array.from({ length: PLAYER_SLOTS }, () => ({ strong: 0, weak: 0, at: 0 }));

  /** Update slot assignments from the browser's current controllers. */
  assign(pads = connectedPads()): void {
    const present = new Set(pads.map((g) => g.index));
    for (let s = 0; s < PLAYER_SLOTS; s++) if (this.slots[s] !== null && !present.has(this.slots[s]!)) this.slots[s] = null;
    for (const gp of pads) {
      if (this.slots.includes(gp.index)) continue;
      const free = this.slots.indexOf(null);
      if (free >= 0) this.slots[free] = gp.index;
    }
  }

  /** The controller in a player slot, if any. */
  pad(slot: number, pads = connectedPads()): Gamepad | undefined {
    return pads.find((g) => g.index === this.slots[slot]);
  }

  /** Once per animation frame while the game runs: report every slot and play rumble requests. */
  feed(engine: PadEngine): void {
    const pads = connectedPads();
    this.assign(pads);
    const rumble = new Float32Array(engine.HEAPU8.buffer, engine._web_gamepad_rumble(), PLAYER_SLOTS * 2);
    for (let s = 0; s < PLAYER_SLOTS; s++) {
      const gp = this.pad(s, pads);
      if (!gp) {
        engine._web_gamepad(s, 0, 0, 0, 0, 0, 0, 0, 0);
        continue;
      }
      const p = readPad(gp);
      engine._web_gamepad(s, 1, p.buttons, p.lx, p.ly, p.rx, p.ry, p.lt, p.rt);
      this.rumble(s, gp, rumble[2 * s], rumble[2 * s + 1]);
    }
  }

  // The game holds an effect until it stops it; browsers play effects for a set time, so a
  // running effect is renewed before it runs out.
  private rumble(slot: number, gp: Gamepad, strong: number, weak: number): void {
    const haptics = (gp as Gamepad & { vibrationActuator?: Haptics | null }).vibrationActuator;
    if (!haptics?.playEffect) return;
    const last = this.played[slot];
    const now = performance.now();
    const changed = Math.abs(strong - last.strong) > 0.02 || Math.abs(weak - last.weak) > 0.02;
    if (!changed && (strong + weak === 0 || now - last.at < 800)) return;
    Object.assign(last, { strong, weak, at: now });
    const done = (p: Promise<unknown> | undefined) => void p?.catch(() => {});
    if (strong + weak === 0) done(haptics.reset ? haptics.reset() : haptics.playEffect("dual-rumble", { duration: 1, strongMagnitude: 0, weakMagnitude: 0 }));
    else done(haptics.playEffect("dual-rumble", { startDelay: 0, duration: 1000, strongMagnitude: Math.min(1, strong), weakMagnitude: Math.min(1, weak) }));
  }
}
