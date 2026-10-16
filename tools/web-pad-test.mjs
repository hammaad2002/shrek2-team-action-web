#!/usr/bin/env node
// Headless checks of the settings screen and of controller play, with a simulated controller in
// place of navigator.getGamepads() (local development only):
//   settings — defaults written on first open, key and controller rebinding by capture, resolution,
//              substitute Esc key, Revert to defaults; values checked in OPFS registry.txt
//   play     — starts a new game using only the controller (A through the menus, left stick up to
//              walk, Start to pause), captures frames to work/web-frames-pad
//   node tools/web-pad-test.mjs [--port 5174] [--only settings|play] [--seconds 260]
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { chromium } from "playwright-core";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
};
const port = opt("port", "5174");
const only = opt("only", "");
const base = `http://localhost:${port}/`;
let failures = 0;
const check = (ok, what, detail = "") => {
  console.log(`${ok ? "PASS" : "FAIL"} ${what}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures++;
};

// Simulated standard-mapping controller. Its state follows a script keyed by presented frames
// (window.__shrek2.frames) or, before the game runs, window.__padHold (button indices / axes).
const fakePad = (script) => {
  const pad = {
    id: "Test Controller (STANDARD GAMEPAD Vendor: 045e Product: 028e)",
    index: 0,
    connected: true,
    mapping: "standard",
    timestamp: 0,
    axes: [0, 0, 0, 0],
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })),
    vibrationActuator: {
      playEffect: (type, p) => (window.__rumble.push({ type, ...p }), Promise.resolve("complete")),
      reset: () => (window.__rumble.push({ type: "reset" }), Promise.resolve("complete")),
    },
  };
  window.__rumble = [];
  window.__padHold = { buttons: [], axes: [0, 0, 0, 0] };
  const names = { a: 0, b: 1, x: 2, y: 3, lb: 4, rb: 5, lt: 6, rt: 7, back: 8, start: 9, ls: 10, rs: 11, up: 12, down: 13, left: 14, right: 15 };
  const axes = { lx: 0, ly: 1, rx: 2, ry: 3 };
  navigator.getGamepads = () => {
    const f = window.__shrek2?.frames ?? 0;
    const held = new Set(window.__padHold.buttons);
    const ax = [...window.__padHold.axes];
    for (const e of script) {
      if (f < e.frame || f >= e.frame + e.hold) continue;
      if (e.control in names) held.add(names[e.control]);
      else if (e.control in axes) ax[axes[e.control]] = e.value;
    }
    pad.buttons.forEach((b, i) => {
      b.pressed = held.has(i);
      b.value = b.pressed ? 1 : 0;
    });
    pad.axes = ax;
    pad.timestamp = performance.now();
    return [pad, null, null, null];
  };
};
const parseScript = (s) =>
  s.split(",").filter(Boolean).map((item) => {
    const [frame, rest] = item.split(":");
    const [ctl, hold] = rest.replace(/^p0\./, "").split("+");
    const [control, value] = ctl.split("=");
    return { frame: Number(frame), control, value: value === undefined ? 1 : Number(value), hold: Number(hold ?? 4) };
  });

const launch = async (script) => {
  const ctx = await chromium.launchPersistentContext(`work/chrome-profile-${port}`, {
    executablePath: "/usr/bin/google-chrome",
    headless: !args.includes("--headed"),
    viewport: { width: 1280, height: 960 },
    args: ["--autoplay-policy=no-user-gesture-required", "--enable-gpu", "--ignore-gpu-blocklist", "--use-angle=vulkan", "--enable-features=Vulkan", "--enable-unsafe-swiftshader"],
  });
  await ctx.addInitScript(fakePad, parseScript(script));
  const page = ctx.pages()[0] ?? (await ctx.newPage());
  page.on("pageerror", (e) => console.log(`pageerror: ${e.message}`));
  return { ctx, page };
};
const registry = (page) =>
  page.evaluate(async () => {
    try {
      const f = await (await (await (await navigator.storage.getDirectory()).getDirectoryHandle("shrek2")).getFileHandle("registry.txt")).getFile();
      const out = {};
      for (const line of (await f.text()).split("\n")) {
        const [key, name, , value] = line.split("\t");
        if (key?.endsWith("\\settings")) out[name] = value;
      }
      return out;
    } catch {
      return {};
    }
  });

if (!only || only === "settings") {
  const { ctx, page } = await launch("");
  await page.goto(base);
  await page.waitForFunction(() => window.__shrek2?.state === "ready", null, { timeout: 60_000 });
  // Start from no settings at all, as on a first visit.
  await page.evaluate(async () => {
    const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle("shrek2");
    await dir.removeEntry("registry.txt").catch(() => {});
    localStorage.removeItem("shrek2.prefs");
  });
  await page.reload();
  await page.waitForFunction(() => window.__shrek2?.state === "ready", null, { timeout: 60_000 });
  await page.click("#settings");
  await page.locator("#settings-panel").waitFor({ state: "visible" });
  check((await page.locator("#set-resolution option:checked").innerText()) === "1024 × 768", "default resolution 1024 × 768");
  const rows = await page.locator("#set-bindings tbody tr").evaluateAll((trs) => trs.map((tr) => [...tr.querySelectorAll("td")].map((td) => td.querySelector(".binding")?.textContent ?? td.textContent)));
  const want = [
    ["Move Up", "↑", "Left stick ↕"], ["Zoom In", "Num 8", "Right stick ↕"], ["Jump", "Space", "A"], ["Use", "Z", "B"], ["Attack", "X", "X"],
    ["Special", "C", "Y"], ["Start (pause)", "Esc", "Start"], ["Next Character", "A", "RB"], ["Previous Character", "S", "LB"], ["Display HUD", "D", "Back"],
  ];
  for (const w of want) check(rows.some((r) => r.join("|") === w.join("|")), `default binding ${w[0]}`, rows.find((r) => r[0] === w[0])?.join(" | "));
  check(/Test Controller/.test(await page.locator("#set-pad-status").innerText()), "controller listed for player 1", await page.locator("#set-pad-status").innerText());

  // The manual's layout as a preset, then back to the game's.
  await page.click("#set-keys-manual");
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  let presetReg = await registry(page);
  check(presetReg.k0_0 === String(0x11) && presetReg.k0_9 === String(0x21), "manual (W A S D) preset", `k0_0=${presetReg.k0_0} k0_9=${presetReg.k0_9}`);
  await page.click("#set-keys-game");
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  presetReg = await registry(page);
  check(presetReg.k0_0 === String(0xc8) && presetReg.k0_9 === String(0x2c), "game (arrow keys) preset", `k0_0=${presetReg.k0_0} k0_9=${presetReg.k0_9}`);

  // Rebind Jump to J on the keyboard.
  const jump = page.locator("#set-bindings tbody tr", { hasText: "Jump" });
  await jump.locator(".binding[data-kind=key]").click();
  await page.keyboard.press("KeyJ");
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  let reg = await registry(page);
  check((await jump.locator(".binding[data-kind=key]").innerText()) === "J" && reg.k0_8 === String(0x24), "Jump rebound to J", `k0_8=${reg.k0_8}`);

  // Rebind Attack on the controller by pressing RB on the simulated pad.
  const attack = page.locator("#set-bindings tbody tr", { hasText: "Attack" });
  await attack.locator(".binding[data-kind=pad]").click();
  await page.evaluate(() => (window.__padHold.buttons = [5]));
  await page.waitForFunction(() => !document.querySelector(".binding.capturing"), null, { timeout: 5_000 });
  await page.evaluate(() => (window.__padHold.buttons = []));
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  reg = await registry(page);
  const nextChar = page.locator("#set-bindings tbody tr", { hasText: "Next Character" });
  check(
    (await attack.locator(".binding[data-kind=pad]").innerText()) === "RB" && reg.gp0_10 === "16" && reg.gp0_13 === "0" && (await nextChar.locator(".binding[data-kind=pad]").innerText()) === "—",
    "Attack rebound to RB (taken from Next Character)",
    `gp0_10=${reg.gp0_10} gp0_13=${reg.gp0_13}`,
  );

  // Invert the camera: bind Cam Left by pushing the right stick right.
  const camLeft = page.locator("#set-bindings tbody tr", { hasText: "Cam Left" });
  const camRight = page.locator("#set-bindings tbody tr", { hasText: "Cam Right" });
  await camLeft.locator(".binding[data-kind=pad]").click();
  await page.evaluate(() => (window.__padHold.axes = [0, 0, 1, 0]));
  await page.waitForFunction(() => !document.querySelector(".binding.capturing"), null, { timeout: 5_000 });
  await page.evaluate(() => (window.__padHold.axes = [0, 0, 0, 0]));
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  reg = await registry(page);
  check(reg.gp0_6 === "0" && reg.gp0_7 === "6" && /inverted/.test(await camRight.locator(".binding[data-kind=pad]").innerText()), "camera stick inverted by capture", `gp0_6=${reg.gp0_6} gp0_7=${reg.gp0_7}`);

  await page.selectOption("#set-resolution", "1");
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  reg = await registry(page);
  check(reg.resolution === "1", "resolution 800 × 600 saved", `resolution=${reg.resolution}`);
  check(reg.pad0 === "53324750-4144-0001-8000-000000000000" && reg.pad3 === "53324750-4144-0001-8000-000000000003", "controller GUIDs written", `${reg.pad0} … ${reg.pad3}`);

  await page.click("#set-alias");
  await page.keyboard.press("KeyP");
  check((await page.locator("#set-alias").innerText()) === "P" && (await page.evaluate(() => JSON.parse(localStorage.getItem("shrek2.prefs")).pauseAlias)) === "KeyP", "substitute Esc key changed to P");

  await page.click("#set-revert");
  await page.click("#set-revert-yes");
  await page.waitForFunction(() => /Saved/.test(document.getElementById("set-status").textContent));
  reg = await registry(page);
  check(reg.resolution === "2" && reg.k0_8 === String(0x39) && reg.gp0_10 === "13" && reg.gp0_6 === "6" && (await page.locator("#set-alias").innerText()) === "`", "Revert to defaults", `resolution=${reg.resolution} k0_8=${reg.k0_8} gp0_10=${reg.gp0_10}`);
  await page.click("#settings-close");
  check(await page.locator("#play-panel").isVisible(), "Done returns to the play panel");
  await ctx.close();
}

if (!only || only === "play") {
  // The same menu path as the native test (tools: SHREK2_PAD), on a fresh save.
  const script = "3450:a,3700:a,3900:a,4100:a,4300:a,4500:a,5000:a,5600:a,5900:ly=-1+150,6160:start";
  const captures = [5880, 6000, 6060, 6120, 6230];
  const { ctx, page } = await launch(script);
  await page.goto(`${base}?autoplay&resetSave&env=SHREK2_TRACE=input&capture=${captures.join(",")}`);
  const outDir = "work/web-frames-pad";
  rmSync(outDir, { recursive: true, force: true });
  mkdirSync(outDir, { recursive: true });
  const deadline = Date.now() + Number(opt("seconds", "260")) * 1000;
  while (Date.now() < deadline) {
    await page.waitForTimeout(3000);
    const s = await page.evaluate(() => ({ state: window.__shrek2.state, frames: window.__shrek2.frames, caps: Object.keys(window.__shrek2.captures).length }));
    if (s.state === "error" || s.caps >= captures.length) break;
  }
  for (const [frame, url] of Object.entries(await page.evaluate(() => window.__shrek2.captures)))
    writeFileSync(`${outDir}/frame_${String(frame).padStart(5, "0")}.png`, Buffer.from(url.split(",")[1], "base64"));
  const log = await page.evaluate(() => window.__shrek2.log);
  writeFileSync(`${outDir}/engine.log`, log.join("\n"));
  check(log.some((l) => l.includes("input: virtual gamepad 0 connected")), "the game sees player 1's controller");
  check(log.some((l) => l.includes("CreateDevice: 1024x768")), "game started at the default 1024 × 768");
  check((await page.evaluate(() => Object.keys(window.__shrek2.captures).length)) === captures.length, "reached gameplay with the controller", `captures in ${outDir}`);
  await ctx.close();
}

console.log(failures ? `${failures} check(s) failed` : "all checks passed");
process.exit(failures ? 1 : 0);
