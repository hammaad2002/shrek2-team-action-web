/**
 * The settings panel (the original Launcher's options, plus browser-only preferences). Changes are
 * saved to the registry file as they are made and apply the next time the game starts.
 */
import { connectedPads, padName, readPad, type Gamepads } from "./gamepads.ts";
import { KEYS } from "./keys.ts";
import { DEFAULT_PREFS, savePrefs, type Prefs } from "./prefs.ts";
import { loadRegistry, saveRegistry } from "./registry.ts";
import {
  ACTIONS, GAME_KEYS, MANUAL_KEYS, OPT_CLIPPING, OPT_SHADOWS, PLAYERS, RESOLUTIONS, bindKey, bindPad, defaultSettings,
  dikForCode, dikLabel, keyLabel, padBindingLabel, padCodeForAxis, padCodeForButton, prepareSettings, writeSettings,
  type GameSettings,
} from "./settings.ts";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

type Capture = { kind: "key" | "pad" | "alias"; player: number; action: number; el: HTMLElement; label?: string };

export class SettingsPanel {
  private settings: GameSettings = defaultSettings();
  private player = 0;
  private capture: Capture | null = null;
  private saveTimer = 0;
  private padFrame = 0;
  private readonly prefs: Prefs;
  private readonly pads: Gamepads;

  constructor(prefs: Prefs, pads: Gamepads) {
    this.prefs = prefs;
    this.pads = pads;
    this.build();
  }

  async open(): Promise<void> {
    this.settings = await prepareSettings();
    this.render();
    $("settings-panel").hidden = false;
    $("play-panel").hidden = true;
    this.watchPads();
  }

  close(): void {
    this.cancelCapture();
    cancelAnimationFrame(this.padFrame);
    $("settings-panel").hidden = true;
    $("play-panel").hidden = false;
  }

  private build(): void {
    const res = $<HTMLSelectElement>("set-resolution");
    RESOLUTIONS.forEach((r, i) => res.add(new Option(r, String(i))));
    for (const id of ["set-mouse-0", "set-mouse-1", "set-mouse-2"]) {
      const sel = $<HTMLSelectElement>(id);
      sel.add(new Option("Nothing", "0"));
      for (const a of ACTIONS.slice(8)) sel.add(new Option(a.name, String(a.id)));
    }
    const players = $("set-players");
    for (let p = 0; p < PLAYERS; p++) {
      const b = document.createElement("button");
      b.className = "tab";
      b.textContent = `Player ${p + 1}`;
      b.addEventListener("click", () => {
        this.player = p;
        this.cancelCapture();
        this.render();
      });
      players.append(b);
    }
    const change = (id: string, fn: (el: HTMLInputElement & HTMLSelectElement) => void) =>
      $(id).addEventListener("input", (e) => {
        fn(e.target as HTMLInputElement & HTMLSelectElement);
        this.changed();
      });
    change("set-resolution", (el) => (this.settings.resolution = Number(el.value)));
    change("set-shadows", (el) => this.flag(OPT_SHADOWS, el.checked));
    change("set-clipping", (el) => this.flag(OPT_CLIPPING, el.checked));
    change("set-distance", (el) => (this.settings.distance = Number(el.value)));
    change("set-mouse", (el) => (this.settings.mouseEnabled = el.checked));
    change("set-sens", (el) => (this.settings.mouseSensitivity = Number(el.value)));
    change("set-wheel", (el) => (this.settings.mouseWheel = el.checked));
    [0, 1, 2].forEach((i) => change(`set-mouse-${i}`, (el) => (this.settings.mouseButtons[i] = Number(el.value))));
    $("set-alias").addEventListener("click", (e) => this.startCapture({ kind: "alias", player: 0, action: 0, el: e.currentTarget as HTMLElement }));
    const preset = (keys: number[]) => () => {
      this.cancelCapture();
      // The player's keys become the preset; other players keep theirs unless they clash.
      for (let p = 0; p < PLAYERS; p++) if (p !== this.player) this.settings.keys[p] = this.settings.keys[p].map((k) => (keys.includes(k) ? 0 : k));
      this.settings.keys[this.player] = [...keys];
      this.changed();
    };
    $("set-keys-game").addEventListener("click", preset(GAME_KEYS));
    $("set-keys-manual").addEventListener("click", preset(MANUAL_KEYS));
    $("settings-close").addEventListener("click", () => this.close());
    $("set-revert").addEventListener("click", () => ($("set-revert-confirm").hidden = false));
    $("set-revert-no").addEventListener("click", () => ($("set-revert-confirm").hidden = true));
    $("set-revert-yes").addEventListener("click", () => {
      $("set-revert-confirm").hidden = true;
      this.settings = defaultSettings();
      Object.assign(this.prefs, DEFAULT_PREFS);
      savePrefs(this.prefs);
      this.changed();
    });
    window.addEventListener("keydown", (e) => this.onKey(e), true);
    document.addEventListener("pointerdown", (e) => {
      if (this.capture && e.target !== this.capture.el) this.cancelCapture();
    });
  }

  private flag(bit: number, on: boolean): void {
    this.settings.options = on ? this.settings.options | bit : this.settings.options & ~bit;
  }

  private changed(): void {
    this.render();
    clearTimeout(this.saveTimer);
    $("set-status").textContent = "Saving…";
    this.saveTimer = window.setTimeout(async () => {
      try {
        const r = await loadRegistry();
        writeSettings(this.settings, r);
        await saveRegistry(r);
        $("set-status").textContent = "Saved. Changes apply the next time you press Play.";
      } catch (err) {
        $("set-status").textContent = `Could not save: ${err instanceof Error ? err.message : String(err)}`;
      }
    }, 150);
  }

  private render(): void {
    const s = this.settings;
    $<HTMLSelectElement>("set-resolution").value = String(s.resolution);
    $<HTMLInputElement>("set-shadows").checked = !!(s.options & OPT_SHADOWS);
    $<HTMLInputElement>("set-clipping").checked = !!(s.options & OPT_CLIPPING);
    $<HTMLInputElement>("set-distance").value = String(s.distance);
    $<HTMLInputElement>("set-distance").disabled = !(s.options & OPT_CLIPPING);
    $<HTMLInputElement>("set-mouse").checked = s.mouseEnabled;
    $<HTMLInputElement>("set-sens").value = String(s.mouseSensitivity);
    $<HTMLInputElement>("set-wheel").checked = s.mouseWheel;
    [0, 1, 2].forEach((i) => ($<HTMLSelectElement>(`set-mouse-${i}`).value = String(s.mouseButtons[i])));
    $("set-alias").textContent = keyLabel(this.prefs.pauseAlias);
    [...$("set-players").children].forEach((b, p) => b.classList.toggle("on", p === this.player));

    const body = $("set-bindings").querySelector("tbody")!;
    body.replaceChildren();
    ACTIONS.forEach((action, a) => {
      const tr = document.createElement("tr");
      const name = document.createElement("td");
      name.textContent = action.name;
      tr.append(name, this.bindingCell("key", a, dikLabel(s.keys[this.player][a])), this.bindingCell("pad", a, padBindingLabel(s.pads[this.player], a)));
      body.append(tr);
    });
    this.renderPads();
  }

  private bindingCell(kind: "key" | "pad", action: number, label: string): HTMLTableCellElement {
    const td = document.createElement("td");
    const b = document.createElement("button");
    b.className = "binding";
    b.textContent = label;
    b.title = label;
    b.dataset.kind = kind;
    b.dataset.action = String(action);
    b.addEventListener("click", () => this.startCapture({ kind, player: this.player, action, el: b }));
    const clear = document.createElement("button");
    clear.className = "clear";
    clear.textContent = "×";
    clear.title = "Clear";
    clear.addEventListener("click", () => {
      this.cancelCapture();
      const row = (kind === "key" ? this.settings.keys : this.settings.pads)[this.player];
      if (kind === "pad") bindPad(row, action, 0);
      else row[action] = 0;
      this.changed();
    });
    td.append(b, clear);
    return td;
  }

  private renderPads(): void {
    const pads = connectedPads();
    this.pads.assign(pads);
    const gp = this.pads.pad(this.player, pads);
    $("set-pad-status").textContent = gp
      ? `Player ${this.player + 1}'s controller: ${padName(gp)}.`
      : `Player ${this.player + 1} has no controller. Connect one and press any of its buttons (controllers go to players in the order they are connected).`;
  }

  // Keep the controller line current while the panel is open.
  private watchPads(): void {
    let last = "";
    const tick = () => {
      const key = connectedPads().map((g) => `${g.index}:${g.id}`).join("|");
      if (key !== last) {
        last = key;
        this.renderPads();
      }
      this.padFrame = requestAnimationFrame(tick);
    };
    tick();
  }

  private startCapture(c: Capture): void {
    this.cancelCapture();
    this.capture = c;
    c.label = c.el.textContent ?? "";
    c.el.classList.add("capturing");
    c.el.textContent = c.kind === "pad" ? "Press a button or move a stick…" : "Press a key…";
    if (c.kind === "pad") this.capturePad(c);
  }

  private cancelCapture(): void {
    const c = this.capture;
    if (!c) return;
    this.capture = null;
    c.el.classList.remove("capturing");
    c.el.textContent = c.label ?? "";
  }

  private onKey(e: KeyboardEvent): void {
    const c = this.capture;
    if (!c) return;
    e.preventDefault();
    e.stopPropagation();
    if (c.kind === "pad") return this.cancelCapture();
    if (c.kind === "alias") {
      if (e.code !== "Escape") {
        this.prefs.pauseAlias = e.code;
        savePrefs(this.prefs);
        // The substitute Esc cannot also be a game key.
        for (const row of this.settings.keys) {
          const dik = dikForCode(e.code);
          for (let a = 0; a < row.length; a++) if (dik !== undefined && row[a] === dik) row[a] = 0;
        }
      }
      this.capture = null;
      return this.changed();
    }
    const dik = dikForCode(e.code);
    if (dik === undefined || !KEYS[e.code]) return; // a key the game cannot see; keep waiting
    if (e.code === this.prefs.pauseAlias) {
      $("set-status").textContent = `${keyLabel(e.code)} is the substitute Esc key (see Browser below); choose another key.`;
      return;
    }
    bindKey(this.settings.keys[c.player], c.action, dik);
    this.capture = null;
    this.changed();
  }

  // Wait for a controller button press or a stick movement away from where it rested.
  private capturePad(c: Capture): void {
    const rest = new Map(connectedPads().map((g) => [g.index, readPad(g)]));
    const pressedAtStart = new Map(connectedPads().map((g) => [g.index, g.buttons.map((b) => b.pressed)]));
    const poll = () => {
      if (this.capture !== c) return;
      for (const gp of connectedPads()) {
        const before = pressedAtStart.get(gp.index) ?? [];
        for (let i = 0; i < Math.min(16, gp.buttons.length); i++) {
          if (!gp.buttons[i].pressed || before[i]) continue;
          const m = padCodeForButton(i);
          if (m) return this.finishPad(c, m.code, m.sign);
        }
        const base = rest.get(gp.index) ?? { lx: 0, ly: 0, rx: 0, ry: 0 };
        const axes = [gp.axes[0] ?? 0, gp.axes[1] ?? 0, gp.axes[2] ?? 0, gp.axes[3] ?? 0];
        const baseAxes = [base.lx, base.ly, base.rx, base.ry];
        for (let i = 0; i < 4; i++) {
          if (Math.abs(axes[i]) > 0.6 && Math.abs(axes[i] - baseAxes[i]) > 0.5) return this.finishPad(c, padCodeForAxis(i)!, Math.sign(axes[i]));
        }
      }
      requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);
  }

  private finishPad(c: Capture, code: number, sign: number): void {
    bindPad(this.settings.pads[c.player], c.action, code, sign);
    this.capture = null;
    this.changed();
  }
}
