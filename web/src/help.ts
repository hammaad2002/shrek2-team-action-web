/**
 * The controls list: what each key, controller button and mouse input does with the current
 * settings (player 1), plus the browser's own keys. Shown once before the first start, then from
 * the play panel's Controls button or with F1 / the HUD's Controls button in game.
 */
import type { Prefs } from "./prefs.ts";
import { ACTIONS, defaultSettings, dikLabel, keyLabel, padBindingLabel, prepareSettings, type GameSettings } from "./settings.ts";

const SEEN = "shrek2.controlsSeen";

export class Help {
  isOpen = false;
  private closeHandler: (() => void) | null = null;
  private readonly prefs: Prefs;
  private readonly el: HTMLElement;

  constructor(prefs: Prefs) {
    this.prefs = prefs;
    this.el = document.getElementById("help")!;
    document.getElementById("help-close")!.addEventListener("click", () => this.hide());
    this.el.addEventListener("click", (e) => {
      if (e.target === this.el) this.hide();
    });
    // Capture phase: F1 / Esc close the list before the game's input handler sees them.
    window.addEventListener(
      "keydown",
      (e) => {
        if (!this.isOpen || (e.code !== "Escape" && e.code !== "F1")) return;
        e.preventDefault();
        e.stopImmediatePropagation();
        if (!e.repeat) this.hide(); // a held key toggles once
      },
      true,
    );
  }

  get seen(): boolean {
    try {
      return localStorage.getItem(SEEN) === "1";
    } catch {
      return true;
    }
  }

  /**
   * Show the list; `onClose` runs when this showing of it is closed. (A handler kept across
   * showings was consumed by the first close, so the game, paused while the list is open, never
   * resumed after the second.)
   */
  async show(closeLabel = "Close", onClose: (() => void) | null = null): Promise<void> {
    if (this.isOpen) return;
    this.isOpen = true;
    this.closeHandler = onClose;
    const settings = await prepareSettings().catch(() => defaultSettings());
    if (!this.isOpen) return; // closed again while the settings loaded
    this.render(settings, closeLabel);
    // In fullscreen only the fullscreen element's subtree is visible.
    (document.fullscreenElement ?? document.body).append(this.el);
    this.el.hidden = false;
    document.getElementById("help-close")!.focus();
    try {
      localStorage.setItem(SEEN, "1");
    } catch {
      // Shown again next time.
    }
  }

  hide(): void {
    if (!this.isOpen) return;
    this.el.hidden = true;
    this.isOpen = false;
    const done = this.closeHandler;
    this.closeHandler = null;
    done?.();
  }

  private render(s: GameSettings, closeLabel: string): void {
    document.getElementById("help-close")!.textContent = closeLabel;
    const body = this.el.querySelector("tbody")!;
    body.replaceChildren(
      ...ACTIONS.map((a, i) => row(a.name, dikLabel(s.keys[0][i]), padBindingLabel(s.pads[0], i))),
    );
    const action = (id: number) => ACTIONS.find((a) => a.id === id)?.name ?? "";
    const mouse: [string, string][] = [];
    if (s.mouseEnabled) mouse.push(["Move the mouse", "Turn the camera (click the game first to capture the mouse)"]);
    if (s.mouseWheel) mouse.push(["Wheel", "Next / previous character"]);
    ["Left button", "Right button", "Middle button"].forEach((b, i) => {
      if (s.mouseButtons[i]) mouse.push([b, action(s.mouseButtons[i])]);
    });
    if (!mouse.length) mouse.push(["—", "The mouse is not used (see Settings)"]);
    list("help-mouse", mouse);
    const alias = this.prefs.pauseAlias;
    const other: [string, string][] = [
      ["F1", "Show or hide this list (the game waits meanwhile)"],
      ["Arrow keys, Enter, Esc", "Choose, accept and go back in the game's menus"],
    ];
    if (alias) other.push([keyLabel(alias), "Works as Esc where the browser keeps Esc (fullscreen, captured mouse)"]);
    other.push(["Fullscreen", "Button at the top right of the game (move the mouse to show it); in Chrome and Edge hold Esc to leave"]);
    other.push(["Players 2–4", "Controllers join in the order they are connected; keyboard keys for other players are in Settings"]);
    list("help-other", other);
  }
}

function row(...cells: string[]): HTMLTableRowElement {
  const tr = document.createElement("tr");
  for (const c of cells) {
    const td = document.createElement("td");
    td.textContent = c;
    tr.append(td);
  }
  return tr;
}

function list(id: string, items: [string, string][]): void {
  document.getElementById(id)!.replaceChildren(
    ...items.map(([k, v]) => {
      const li = document.createElement("li");
      const b = document.createElement("strong");
      b.textContent = k;
      li.append(b, ` — ${v}`);
      return li;
    }),
  );
}
