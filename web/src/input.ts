/**
 * Keyboard and mouse input for the engine: DOM events → Windows virtual keys / DirectInput scan
 * codes (keys.ts) and window-relative mouse messages, with pointer lock for the mouse camera.
 */
import { KEYS } from "./keys.ts";

export interface InputEngine {
  _web_key(kind: number, vk: number, dik: number, ch: number): void;
  _web_mouse_move(x: number, y: number, dx: number, dy: number): void;
  _web_mouse_button(button: number, down: number, x: number, y: number): void;
  _web_mouse_wheel(dy: number): void;
  _web_focus(focused: number): void;
}

export interface InputOptions {
  /** KeyboardEvent.code that also acts as Escape (browsers reserve Esc in fullscreen/pointer lock). */
  pauseAlias: string;
  /** Lock the pointer on click so mouse movement turns the camera (the game's "mouse camera"). */
  mouseCapture: boolean;
}

export class Input {
  options: InputOptions;
  private readonly engine: InputEngine;
  private readonly canvas: HTMLCanvasElement;
  /** Buttons whose press only captured the pointer (their release is not sent either). */
  private readonly swallowed = new Set<number>();

  constructor(engine: InputEngine, canvas: HTMLCanvasElement, options: InputOptions) {
    this.engine = engine;
    this.canvas = canvas;
    this.options = options;
    window.addEventListener("keydown", (e) => this.key(e, true));
    window.addEventListener("keyup", (e) => this.key(e, false));
    canvas.addEventListener("mousemove", (e) => {
      const [x, y] = this.pos(e);
      engine._web_mouse_move(x, y, e.movementX, e.movementY);
    });
    canvas.addEventListener("mousedown", (e) => {
      // The click that captures the pointer is not a game click (mouse buttons may be bound to
      // actions such as Attack).
      if (this.options.mouseCapture && document.pointerLockElement !== canvas) {
        this.swallowed.add(e.button);
        this.lockPointer();
        return;
      }
      const [x, y] = this.pos(e);
      engine._web_mouse_button(button(e.button), 1, x, y);
    });
    canvas.addEventListener("mouseup", (e) => {
      if (this.swallowed.delete(e.button)) return;
      const [x, y] = this.pos(e);
      engine._web_mouse_button(button(e.button), 0, x, y);
    });
    canvas.addEventListener("wheel", (e) => engine._web_mouse_wheel(e.deltaY < 0 ? 1 : -1), { passive: true });
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    window.addEventListener("focus", () => engine._web_focus(1));
    window.addEventListener("blur", () => engine._web_focus(0));
  }

  private key(e: KeyboardEvent, down: boolean): void {
    const code = e.code === this.options.pauseAlias ? "Escape" : e.code;
    const key = KEYS[code];
    if (!key) return;
    // Let the browser keep its own shortcuts for keys the game never uses (e.g. F5, F11, F12).
    e.preventDefault();
    if (down && e.repeat) return;
    this.engine._web_key(down ? 0 : 1, key[0], key[1], 0);
    if (down && e.key.length === 1 && code === e.code) this.engine._web_key(2, 0, 0, e.key.charCodeAt(0));
  }

  /** Window-relative coordinates in the game's 640x480 client space. */
  private pos(e: MouseEvent): [number, number] {
    const r = this.canvas.getBoundingClientRect();
    return [Math.round(((e.clientX - r.left) / r.width) * 640), Math.round(((e.clientY - r.top) / r.height) * 480)];
  }

  get pointerLocked(): boolean {
    return document.pointerLockElement === this.canvas;
  }

  lockPointer(): void {
    try {
      // unadjustedMovement gives raw deltas where supported; fall back to a plain request.
      const p = (this.canvas.requestPointerLock as (o?: object) => Promise<void> | void).call(this.canvas, { unadjustedMovement: true });
      if (p && typeof (p as Promise<void>).catch === "function") (p as Promise<void>).catch(() => this.canvas.requestPointerLock());
    } catch {
      this.canvas.requestPointerLock();
    }
  }
}

function button(b: number): number {
  return b === 0 ? 0 : b === 2 ? 1 : 2;
}
