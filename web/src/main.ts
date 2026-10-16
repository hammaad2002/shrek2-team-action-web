/**
 * Page shell: installs the game from the player's disc image into OPFS, then starts the engine
 * (WebAssembly runtime on worker threads) and drives rendering, input and audio from the page.
 *
 * Development switches (URL query): `devIso` installs from the dev server's local disc image,
 * `autoplay` starts without waiting for a click, `keys=frame:Code[+hold],...` presses keys at
 * presented frames, `capture=frame,...` keeps PNG snapshots in `window.__shrek2.captures`,
 * `resetSave` deletes saves first, `env=K=V;...` sets engine environment variables.
 */
import { INSTALL_DIR, cacheName, sha256 } from "./cache.ts";
import { canLockEscape, enterFullscreen, exitFullscreen, isFullscreen, watchFullscreen } from "./fullscreen.ts";
import { Input } from "./input.ts";
import { KEYS } from "./keys.ts";
import { loadPrefs } from "./prefs.ts";
import { applyImport, exportSaves, listSaves, planImport, type ImportPlan } from "./saves.ts";

const params = new URLSearchParams(location.search);
// The player engine contains no game code: the game is translated in this browser. `devEngine`
// selects the local C-backend build (development only, served from work/ by the dev server).
const DEV_ENGINE = params.has("devEngine");
const ENGINE_URL = DEV_ENGINE ? "/dev-engine/shrek2.js" : "/engine/shrek2_player.js";

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

async function gameDir(): Promise<FileSystemDirectoryHandle> {
  return (await navigator.storage.getDirectory()).getDirectoryHandle(INSTALL_DIR);
}

/** The cached translation of the installed exe, or null when it must be (re)translated. */
async function cachedTranslation(): Promise<Uint8Array | null> {
  const root = await gameDir();
  const exe = await (await (await root.getDirectoryHandle("Game")).getFileHandle("Shrek 2.exe")).getFile();
  const name = cacheName(await sha256(await exe.arrayBuffer()));
  try {
    const f = await (await (await root.getDirectoryHandle("cache")).getFileHandle(name)).getFile();
    return new Uint8Array(await f.arrayBuffer());
  } catch {
    return null;
  }
}

function runTranslate(): Promise<void> {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL("./translate-worker.ts", import.meta.url), { type: "module" });
    progress.hidden = false;
    setState("translating", "Translating the game for this browser…");
    worker.onmessage = (e: MessageEvent) => {
      const m = e.data;
      if (m.type === "progress") {
        progress.max = m.total || 1;
        progress.value = m.done;
        status.textContent =
          m.phase === "discover" ? "Analysing the game program…" : m.phase === "translate" ? `Translating ${m.done} / ${m.total} functions…` : "Finishing…";
      } else if (m.type === "done") {
        worker.terminate();
        progress.hidden = true;
        log(`Translated ${m.functions} functions into ${(m.bytes / 1048576).toFixed(1)} MB of WebAssembly in ${m.seconds.toFixed(1)} s`);
        resolve();
      } else if (m.type === "error") {
        worker.terminate();
        reject(new Error(m.message));
      }
    };
    worker.onerror = (e) => {
      worker.terminate();
      reject(new Error(`translator crashed: ${e.message}`));
    };
    worker.postMessage({});
  });
}

/** Function entry addresses from the module's "shrek2.funcs" custom section. */
function functionList(wasm: Uint8Array): Uint32Array {
  let i = 8;
  const leb = () => {
    let v = 0, shift = 0, b: number;
    do {
      b = wasm[i++];
      v |= (b & 0x7f) << shift;
      shift += 7;
    } while (b & 0x80);
    return v >>> 0;
  };
  while (i < wasm.length) {
    const id = wasm[i++];
    const size = leb();
    const end = i + size;
    if (id === 0) {
      const n = leb();
      const name = new TextDecoder().decode(wasm.subarray(i, i + n));
      i += n;
      if (name === "shrek2.funcs") {
        const dv = new DataView(wasm.buffer, wasm.byteOffset + i, end - i);
        const count = dv.getUint32(0, true);
        return Uint32Array.from({ length: count }, (_, k) => dv.getUint32(4 + 4 * k, true));
      }
    }
    i = end;
  }
  throw new Error("translated module has no function list");
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
  _malloc(n: number): number;
  _web_game_alloc(n: number): number;
  _web_game_register(ptr: number, count: number): void;
}

async function resetSaves() {
  const opfs = await navigator.storage.getDirectory();
  const game = await (await opfs.getDirectoryHandle(INSTALL_DIR)).getDirectoryHandle("Game");
  const save = await game.getDirectoryHandle("Save", { create: true });
  for await (const name of (save as unknown as { keys(): AsyncIterable<string> }).keys()) await save.removeEntry(name);
}

async function play() {
  setState("starting", "Starting…");
  if (params.has("resetSave")) await resetSaves();
  let game: Uint8Array | null = null;
  if (!DEV_ENGINE) {
    game = await cachedTranslation();
    if (!game) {
      await runTranslate();
      game = await cachedTranslation();
      if (!game) throw new Error("translation finished but its output is missing");
    }
  }
  $("play-panel").hidden = true;
  const canvas = $<HTMLCanvasElement>("canvas");
  $("game").classList.add("on");

  // Audio must be created during a user gesture (or it starts suspended).
  const audio = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
  await audio.audioWorklet.addModule("/audio-worklet.js");

  // Absolute URL: the engine is a static file, not a module for the dev server to transform.
  const { default: createShrek2 } = await import(/* @vite-ignore */ new URL(ENGINE_URL, location.href).href);
  // Development: engine environment variables (SHREK2_WATCHDOG, SHREK2_TRACE, ...) from `env=K=V;K=V`.
  const env = Object.fromEntries(
    (params.get("env") ?? "")
      .split(";")
      .filter(Boolean)
      .map((kv) => [kv.slice(0, kv.indexOf("=")), kv.slice(kv.indexOf("=") + 1)]),
  );
  const engine: Engine = await createShrek2({
    canvas,
    preRun: [(m: { ENV: Record<string, string> }) => Object.assign(m.ENV, env)],
    print: (s: string) => dev.log.push(s),
    printErr: (s: string) => {
      dev.log.push(s);
      if (s.includes("[fatal]") || s.includes("[error]")) console.error(s);
    },
  });

  // Canvas size follows its element (device pixels); the engine letterboxes the 4:3 image.
  const resize = () => {
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(canvas.clientWidth * dpr));
    canvas.height = Math.max(1, Math.round(canvas.clientHeight * dpr));
    engine._web_canvas_size(canvas.width, canvas.height);
  };
  resize();
  window.addEventListener("resize", resize);
  new ResizeObserver(resize).observe(canvas);

  if (!engine._web_render_init(engine.stringToNewUTF8("#canvas"))) throw new Error("WebGL 2 is not available");

  const header = engine._web_audio_ring_create(audio.sampleRate);
  const node = new AudioWorkletNode(audio, "ring-player", {
    numberOfInputs: 0,
    outputChannelCount: [2],
    processorOptions: { buffer: engine.HEAPU8.buffer, header },
  });
  node.connect(audio.destination);
  void audio.resume();

  if (game) {
    // Hand the translated module and its function list to the engine (instantiated per worker).
    const ptr = engine._web_game_alloc(game.length);
    engine.HEAPU8.set(game, ptr);
    const funcs = functionList(game);
    const fptr = engine._malloc(funcs.length * 4);
    new Uint32Array(engine.HEAPU8.buffer, fptr, funcs.length).set(funcs);
    engine._web_game_register(fptr, funcs.length);
  }

  const prefs = loadPrefs();
  const input = new Input(engine, canvas, { pauseAlias: prefs.pauseAlias, mouseCapture: true });
  attachHud(input);

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

/** The overlay with the fullscreen button and input hints; shown while the mouse moves. */
function attachHud(input: Input) {
  const hud = $("hud");
  const hint = $("hint");
  const button = $<HTMLButtonElement>("fullscreen");
  const alias = input.options.pauseAlias === "Backquote" ? "` (left of 1)" : input.options.pauseAlias;
  let timer = 0;
  const show = (ms: number) => {
    hud.classList.add("show");
    clearTimeout(timer);
    timer = window.setTimeout(() => hud.classList.remove("show"), ms);
  };
  const update = () => {
    const fs = isFullscreen();
    button.textContent = fs ? "Exit fullscreen" : "Fullscreen";
    const tips = [];
    if (input.options.mouseCapture && !input.pointerLocked) tips.push("Click the game to steer the camera with the mouse.");
    if (fs && canLockEscape) tips.push("Hold Esc to leave fullscreen.");
    else if (fs || input.pointerLocked) tips.push(`Esc is taken by the browser here: press ${alias} for the game's Esc.`);
    hint.textContent = tips.join(" ");
  };
  button.addEventListener("click", () => {
    void (isFullscreen() ? exitFullscreen() : enterFullscreen($("game"))).catch((err: unknown) =>
      log(`Fullscreen is not available: ${err instanceof Error ? err.message : String(err)}`, true),
    );
    $("canvas").focus();
  });
  watchFullscreen(() => {
    update();
    show(4000);
  });
  document.addEventListener("pointerlockchange", () => {
    update();
    if (input.pointerLocked) hud.classList.remove("show");
    else show(4000);
  });
  $("game").addEventListener("mousemove", () => {
    if (!input.pointerLocked) show(2500);
  });
  update();
  show(6000);
}

// ---- Save backups ------------------------------------------------------------------------------

const saveStatus = () => $("save-status");

async function refreshSaves() {
  const saves = await listSaves();
  $<HTMLButtonElement>("save-export").disabled = saves.length === 0;
  if (!saveStatus().dataset.sticky) {
    saveStatus().textContent = saves.length
      ? `${saves.length} save ${saves.length === 1 ? "slot" : "slots"} in this browser, last saved ${saves.reduce((a, b) => (a.date > b.date ? a : b)).date.toLocaleString()}.`
      : "No saved games in this browser yet.";
  }
}

function sayAboutSaves(text: string, error = false) {
  const el = saveStatus();
  el.textContent = text;
  el.dataset.sticky = "1";
  el.classList.toggle("error", error);
}

function attachSaves() {
  let pending: ImportPlan | null = null;
  const confirmBox = $("save-confirm");
  const fileInput = $<HTMLInputElement>("save-import");
  // One backup or restore at a time.
  const busy = (on: boolean) => {
    for (const id of ["save-export", "save-import", "save-confirm-yes", "save-confirm-no"]) $<HTMLButtonElement>(id).disabled = on;
    $("save-import-label").style.opacity = on ? "0.5" : "";
    if (!on) void refreshSaves();
  };
  const apply = async (plan: ImportPlan) => {
    sayAboutSaves("Restoring…");
    busy(true);
    try {
      await applyImport(plan);
      sayAboutSaves(`Restored ${plan.files.map((f) => f.name).join(", ")}.`);
    } finally {
      busy(false);
    }
  };
  $("save-export").addEventListener("click", async () => {
    try {
      const zip = await exportSaves();
      if (!zip) return sayAboutSaves("There are no saved games to back up yet.");
      const a = document.createElement("a");
      a.href = URL.createObjectURL(zip);
      a.download = `shrek2-saves-${new Date().toISOString().slice(0, 10)}.zip`;
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 60_000);
      sayAboutSaves(`Downloaded ${a.download}.`);
    } catch (err) {
      sayAboutSaves(`Backup failed: ${err instanceof Error ? err.message : String(err)}`, true);
    }
  });
  fileInput.addEventListener("change", async () => {
    const files = [...(fileInput.files ?? [])];
    fileInput.value = "";
    if (!files.length) return;
    confirmBox.hidden = true;
    pending = null;
    sayAboutSaves("Checking the files…");
    let plan: ImportPlan;
    busy(true);
    try {
      plan = await planImport(files);
    } catch (err) {
      sayAboutSaves(`Nothing restored: ${err instanceof Error ? err.message : String(err)}`, true);
      return;
    } finally {
      busy(false);
    }
    if (plan.overwrites.length) {
      pending = plan;
      $("save-confirm-text").textContent = `This replaces ${plan.overwrites.join(", ")} in this browser. Back up first if you may want them again.`;
      sayAboutSaves("Waiting for your confirmation.");
      confirmBox.hidden = false;
      return;
    }
    try {
      await apply(plan);
    } catch (err) {
      sayAboutSaves(`Restore failed: ${err instanceof Error ? err.message : String(err)}`, true);
    }
  });
  $("save-confirm-yes").addEventListener("click", async () => {
    confirmBox.hidden = true;
    const plan = pending;
    pending = null;
    if (!plan) return;
    try {
      await apply(plan);
    } catch (err) {
      sayAboutSaves(`Restore failed: ${err instanceof Error ? err.message : String(err)}`, true);
    }
  });
  $("save-confirm-no").addEventListener("click", () => {
    confirmBox.hidden = true;
    pending = null;
    sayAboutSaves("Nothing was changed.");
  });
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
    void refreshSaves();
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
  attachSaves();

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
