/**
 * Page shell: installs the game from the player's disc image into OPFS, then starts the engine
 * (WebAssembly runtime on worker threads) and drives rendering, input and audio from the page.
 *
 * Development switches (URL query): `devIso` installs from the dev server's local disc image,
 * `autoplay` starts without waiting for a click, `keys=frame:Code[+hold],...` presses keys at
 * presented frames, `capture=frame,...` keeps PNG snapshots in `window.__shrek2.captures`.
 */
import { KEYS } from "./keys.ts";

const INSTALL_DIR = "shrek2";
const ENGINE_URL = "/engine/shrek2.js";
const params = new URLSearchParams(location.search);

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const status = $("status");
const progress = $<HTMLProgressElement>("progress");
const logEl = $("log");

function log(msg: string, error = false) {
  const line = document.createElement("div");
  line.textContent = msg;
  if (error) line.className = "error";
  logEl.append(line);
  logEl.scrollTop = logEl.scrollHeight;
  (error ? console.error : console.log)(msg);
}

interface DevState {
  frames: number;
  captures: Record<number, string>;
  log: string[];
  state: string;
}
const dev: DevState = { frames: 0, captures: {}, log: [], state: "init" };
(window as unknown as { __shrek2: DevState }).__shrek2 = dev;

function setState(s: string, text: string) {
  dev.state = s;
  status.textContent = text;
}

async function installed(): Promise<boolean> {
  try {
    const opfs = await navigator.storage.getDirectory();
    const root = await opfs.getDirectoryHandle(INSTALL_DIR);
    const game = await root.getDirectoryHandle("Game");
    await game.getFileHandle("Shrek 2.exe");
    return true;
  } catch {
    return false;
  }
}

function runInstall(file: Blob): Promise<void> {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL("./install-worker.ts", import.meta.url), { type: "module" });
    progress.hidden = false;
    setState("installing", "Unpacking your disc image…");
    worker.onmessage = (e: MessageEvent) => {
      const m = e.data;
      if (m.type === "progress") {
        progress.max = m.total || 1;
        progress.value = m.done;
        status.textContent = `Unpacking ${m.done} / ${m.total} files…`;
      } else if (m.type === "done") {
        worker.terminate();
        progress.hidden = true;
        log(`Installed ${m.written} files (${(m.bytes / 1048576).toFixed(0)} MB, ${m.verified} verified) in ${m.seconds.toFixed(1)} s`);
        for (const x of m.mismatches) log(`warning: ${x}`, true);
        resolve();
      } else if (m.type === "error") {
        worker.terminate();
        reject(new Error(m.message));
      }
    };
    worker.postMessage({ file });
  });
}

// ---- Engine ------------------------------------------------------------------------------------

interface Engine {
  HEAPU8: Uint8Array;
  callMain(args: string[]): void;
  _web_render_init(selector: number): number;
  _web_render_tick(): number;
  _web_audio_ring_create(rate: number): number;
  _web_canvas_size(w: number, h: number): void;
  _web_key(kind: number, vk: number, dik: number, ch: number): void;
  _web_mouse_move(x: number, y: number, dx: number, dy: number): void;
  _web_mouse_button(button: number, down: number, x: number, y: number): void;
  _web_mouse_wheel(dy: number): void;
  _web_focus(focused: number): void;
  stringToNewUTF8(s: string): number;
}

async function play() {
  setState("starting", "Starting…");
  $("play-panel").hidden = true;
  const canvas = $<HTMLCanvasElement>("canvas");
  $("game").classList.add("on");

  // Audio must be created during a user gesture (or it starts suspended).
  const audio = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
  await audio.audioWorklet.addModule("/audio-worklet.js");

  const { default: createShrek2 } = await import(/* @vite-ignore */ ENGINE_URL);
  const engine: Engine = await createShrek2({
    canvas,
    print: (s: string) => dev.log.push(s),
    printErr: (s: string) => {
      dev.log.push(s);
      if (s.includes("[fatal]") || s.includes("[error]")) console.error(s);
    },
  });

  // Canvas size follows the window (device pixels); the engine letterboxes the 4:3 image.
  const resize = () => {
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(canvas.clientWidth * dpr));
    canvas.height = Math.max(1, Math.round(canvas.clientHeight * dpr));
    engine._web_canvas_size(canvas.width, canvas.height);
  };
  resize();
  window.addEventListener("resize", resize);

  if (!engine._web_render_init(engine.stringToNewUTF8("#canvas"))) throw new Error("WebGL 2 is not available");

  const header = engine._web_audio_ring_create(audio.sampleRate);
  const node = new AudioWorkletNode(audio, "ring-player", {
    numberOfInputs: 0,
    outputChannelCount: [2],
    processorOptions: { buffer: engine.HEAPU8.buffer, header },
  });
  node.connect(audio.destination);
  void audio.resume();

  attachInput(engine, canvas);

  // Scripted keys for automated tests: frame:Code[+hold]
  const script = (params.get("keys") ?? "")
    .split(",")
    .filter(Boolean)
    .map((s) => {
      const [f, rest] = s.split(":");
      const [code, hold] = rest.split("+");
      return { frame: Number(f), code, hold: Number(hold ?? 4), down: false, up: false };
    });
  const captures = new Set((params.get("capture") ?? "").split(",").filter(Boolean).map(Number));

  const tick = () => {
    if (engine._web_render_tick()) {
      dev.frames++;
      if (captures.has(dev.frames)) dev.captures[dev.frames] = canvas.toDataURL("image/png");
      for (const k of script) {
        const key = KEYS[k.code];
        if (!key) continue;
        if (!k.down && dev.frames >= k.frame) {
          k.down = true;
          engine._web_key(0, key[0], key[1], 0);
        } else if (k.down && !k.up && dev.frames >= k.frame + k.hold) {
          k.up = true;
          engine._web_key(1, key[0], key[1], 0);
        }
      }
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);

  setState("running", "Running");
  canvas.focus();
  engine.callMain([`/opfs/${INSTALL_DIR}`]);
}

function attachInput(engine: Engine, canvas: HTMLCanvasElement) {
  const onKey = (down: boolean) => (e: KeyboardEvent) => {
    const key = KEYS[e.code];
    if (!key) return;
    e.preventDefault();
    if (down && e.repeat) return;
    engine._web_key(down ? 0 : 1, key[0], key[1], 0);
    if (down && e.key.length === 1) engine._web_key(2, 0, 0, e.key.charCodeAt(0));
  };
  window.addEventListener("keydown", onKey(true));
  window.addEventListener("keyup", onKey(false));
  const pos = (e: MouseEvent) => {
    const r = canvas.getBoundingClientRect();
    return [Math.round(((e.clientX - r.left) / r.width) * 640), Math.round(((e.clientY - r.top) / r.height) * 480)];
  };
  canvas.addEventListener("mousemove", (e) => {
    const [x, y] = pos(e);
    engine._web_mouse_move(x, y, e.movementX, e.movementY);
  });
  const button = (b: number) => (b === 0 ? 0 : b === 2 ? 1 : 2);
  canvas.addEventListener("mousedown", (e) => {
    const [x, y] = pos(e);
    engine._web_mouse_button(button(e.button), 1, x, y);
  });
  canvas.addEventListener("mouseup", (e) => {
    const [x, y] = pos(e);
    engine._web_mouse_button(button(e.button), 0, x, y);
  });
  canvas.addEventListener("wheel", (e) => engine._web_mouse_wheel(e.deltaY < 0 ? 1 : -1), { passive: true });
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  window.addEventListener("focus", () => engine._web_focus(1));
  window.addEventListener("blur", () => engine._web_focus(0));
}

// ---- Startup -----------------------------------------------------------------------------------

async function main() {
  if (!crossOriginIsolated) {
    setState("error", "This page must be served with cross-origin isolation (COOP/COEP headers).");
    return;
  }
  if (!("storage" in navigator) || !navigator.storage.getDirectory) {
    setState("error", "This browser does not support the Origin Private File System.");
    return;
  }
  void navigator.storage.persist?.();

  const showPlay = () => {
    setState("ready", "Ready.");
    $("install-panel").hidden = true;
    $("play-panel").hidden = false;
    if (params.has("autoplay")) void play().catch(fail);
  };
  const askInstall = () => {
    setState("need-install", "No installed copy found in this browser.");
    $("install-panel").hidden = false;
    $("play-panel").hidden = true;
  };
  const fail = (err: unknown) => {
    setState("error", "Something went wrong.");
    log(err instanceof Error ? `${err.message}\n${err.stack ?? ""}` : String(err), true);
  };

  $<HTMLInputElement>("iso").addEventListener("change", async (e) => {
    const file = (e.target as HTMLInputElement).files?.[0];
    if (!file) return;
    try {
      await runInstall(file);
      showPlay();
    } catch (err) {
      fail(err);
    }
  });
  $("play").addEventListener("click", () => void play().catch(fail));
  $("reinstall").addEventListener("click", askInstall);

  if (await installed()) showPlay();
  else if (params.has("devIso")) {
    try {
      setState("downloading", "Fetching the development disc image…");
      const blob = await (await fetch("/dev-iso")).blob();
      await runInstall(blob);
      showPlay();
    } catch (err) {
      fail(err);
    }
  } else askInstall();
}

void main();
