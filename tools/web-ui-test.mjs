#!/usr/bin/env node
// Headless checks of the page shell around the game (local development only):
//   saves   — back up, delete, restore (ZIP from the page, a deflated ZIP with folders, single
//             files), overwrite confirmation, rejection of other files
//   view    — game view HUD, cursor hidden by the game, fullscreen, pointer lock, pause-key alias
//   help    — settings written by the first page version migrate to the game's keys; the controls
//             list before the first start, and with F1 / the HUD button in game (game paused)
//   node tools/web-ui-test.mjs [--port 5174] [--only saves|view|help] [--headed]
// Needs the dev server (cd web && npx vite --port 5174) and an installed game in that port's
// profile (run tools/web-smoke.mjs with devIso first). Saves must exist for the saves part.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { chromium } from "playwright-core";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
};
const port = opt("port", "5174");
const only = opt("only", "");
const base = `http://localhost:${port}/`;
const tmp = "work/ui-test";
rmSync(tmp, { recursive: true, force: true });
mkdirSync(tmp, { recursive: true });

let failures = 0;
const check = (ok, what, detail = "") => {
  console.log(`${ok ? "PASS" : "FAIL"} ${what}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures++;
};
const sha = (b) => createHash("sha256").update(b).digest("hex").slice(0, 16);

const ctx = await chromium.launchPersistentContext(`work/chrome-profile-${port}`, {
  executablePath: "/usr/bin/google-chrome",
  headless: !args.includes("--headed"),
  viewport: { width: 1280, height: 960 },
  acceptDownloads: true,
  args: ["--autoplay-policy=no-user-gesture-required", "--enable-gpu", "--ignore-gpu-blocklist", "--use-angle=vulkan", "--enable-features=Vulkan", "--enable-unsafe-swiftshader"],
});
const page = ctx.pages()[0] ?? (await ctx.newPage());
page.on("pageerror", (e) => console.log(`pageerror: ${e.message}`));

const waitState = (s, timeout = 60_000) => page.waitForFunction((want) => window.__shrek2?.state === want, s, { timeout });
const opfsSaves = () =>
  page.evaluate(async () => {
    const out = {};
    try {
      const dir = await (await (await (await navigator.storage.getDirectory()).getDirectoryHandle("shrek2")).getDirectoryHandle("Game")).getDirectoryHandle("Save");
      for await (const [name, h] of dir.entries()) if (h.kind === "file") out[name] = [...new Uint8Array(await (await h.getFile()).arrayBuffer())];
    } catch {}
    return out;
  });
const deleteSaves = () =>
  page.evaluate(async () => {
    const dir = await (await (await (await navigator.storage.getDirectory()).getDirectoryHandle("shrek2")).getDirectoryHandle("Game")).getDirectoryHandle("Save");
    for await (const name of dir.keys()) await dir.removeEntry(name);
  });
const statusText = () => page.locator("#save-status").innerText();
const waitStatus = (re) => page.waitForFunction((src) => new RegExp(src).test(document.getElementById("save-status").textContent), re.source, { timeout: 15_000 });

if (!only || only === "saves") {
  await page.goto(base);
  await waitState("ready");
  const before = await opfsSaves();
  const names = Object.keys(before).sort();
  console.log(`saves in OPFS: ${names.map((n) => `${n} (${before[n].length} B, ${sha(Buffer.from(before[n]))})`).join(", ") || "none"}`);
  if (!names.length) {
    check(false, "saves exist to back up", "play until the game saves (e.g. web-smoke.mjs with resetSave and the new-game key script)");
  } else {
    check(/save slot/.test(await statusText()), "status lists the saves", await statusText());

    // Back up through the page's button (a real download).
    const [download] = await Promise.all([page.waitForEvent("download"), page.click("#save-export")]);
    const zipPath = join(tmp, download.suggestedFilename());
    await download.saveAs(zipPath);
    check(/^shrek2-saves-\d{4}-\d\d-\d\d\.zip$/.test(download.suggestedFilename()), "backup file name", download.suggestedFilename());
    // Independent check of the archive (Python's zipfile verifies CRCs on read).
    const listing = JSON.parse(
      execFileSync("python3", ["-I", "-c", "import sys,zipfile,json,hashlib;z=zipfile.ZipFile(sys.argv[1]);print(json.dumps({i.filename:hashlib.sha256(z.read(i)).hexdigest()[:16] for i in z.infolist()}))", zipPath]).toString(),
    );
    check(
      names.every((n) => listing[`Save/${n}`] === sha(Buffer.from(before[n]))) && Object.keys(listing).length === names.length,
      "backup ZIP holds every save byte-for-byte",
      JSON.stringify(listing),
    );

    // Lose the saves, then restore them from the backup.
    await deleteSaves();
    await page.reload();
    await waitState("ready");
    await waitStatus(/No saved games/);
    check(await page.locator("#save-export").isDisabled(), "backup disabled with no saves");
    await page.setInputFiles("#save-import", zipPath);
    await waitStatus(/Restored/);
    const restored = await opfsSaves();
    check(names.every((n) => sha(Buffer.from(restored[n] ?? [])) === sha(Buffer.from(before[n]))), "restore writes the original bytes", await statusText());
    check(!(await page.locator("#save-export").isDisabled()), "backup enabled again");

    // Restoring over existing saves asks first; Cancel changes nothing.
    await page.setInputFiles("#save-import", zipPath);
    await page.locator("#save-confirm").waitFor({ state: "visible" });
    check(/replaces/.test(await page.locator("#save-confirm-text").innerText()), "overwrite needs confirmation", await page.locator("#save-confirm-text").innerText());
    await page.click("#save-confirm-no");
    await waitStatus(/Nothing was changed/);

    // A deflated ZIP with the saves nested in folders (as a user's own archive might be), confirmed.
    const nested = join(tmp, "nested.zip");
    const savesDir = join(tmp, "Shrek 2 Team Action", "Game", "Save");
    mkdirSync(savesDir, { recursive: true });
    for (const n of names) writeFileSync(join(savesDir, n), Buffer.from(before[n]));
    execFileSync("python3", ["-I", "-c", "import sys,zipfile,os;z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED)\nfor n in sys.argv[3:]: z.write(os.path.join(sys.argv[2],n),'Shrek 2 Team Action/Game/Save/'+n)\nz.close()", nested, savesDir, ...names]);
    await deleteSaves();
    await page.setInputFiles("#save-import", nested);
    await waitStatus(/Restored/);
    const fromNested = await opfsSaves();
    check(names.every((n) => sha(Buffer.from(fromNested[n] ?? [])) === sha(Buffer.from(before[n]))), "deflated ZIP with folders restores", await statusText());

    // Individual save files from a Windows install, replacing the existing slot after confirmation.
    await page.setInputFiles("#save-import", join(savesDir, names[0]));
    await page.locator("#save-confirm").waitFor({ state: "visible" });
    await page.click("#save-confirm-yes");
    await waitStatus(/Restored/);
    check((await statusText()) === `Restored ${names[0]}.`, "single save file restores after confirmation", await statusText());

    // Anything else is refused without touching the saves.
    const bogus = join(tmp, "notes.txt");
    writeFileSync(bogus, "not a save");
    await page.setInputFiles("#save-import", bogus);
    await waitStatus(/Nothing restored/);
    check(await page.locator("#save-status").evaluate((e) => e.classList.contains("error")), "other files are refused", await statusText());
    const empty = join(tmp, "save.bin");
    writeFileSync(empty, "");
    await page.setInputFiles("#save-import", empty);
    await waitStatus(/unexpected size/);
    check(true, "empty save file is refused", await statusText());
    const after = await opfsSaves();
    check(names.every((n) => sha(Buffer.from(after[n] ?? [])) === sha(Buffer.from(before[n]))), "saves intact after refused imports");
  }
}

if (!only || only === "view") {
  // The game view: run the game (input trace on) and exercise the HUD.
  await page.goto(`${base}?autoplay&env=SHREK2_TRACE=input,file`);
  await waitState("running", 120_000);
  await page.waitForFunction(() => window.__shrek2.frames > 60, null, { timeout: 120_000 });
  check(await page.locator("#hud").evaluate((e) => e.classList.contains("show")), "HUD shown when the game starts", await page.locator("#hint").innerText());
  await page.waitForFunction(() => document.getElementById("canvas").style.cursor === "none", null, { timeout: 30_000 }).then(
    () => check(true, "game's ShowCursor(FALSE) hides the cursor over the canvas"),
    () => check(false, "game's ShowCursor(FALSE) hides the cursor over the canvas"),
  );

  // Fullscreen button (needs the HUD visible: move the mouse first).
  await page.mouse.move(640, 300);
  await page.mouse.move(700, 320);
  await page.click("#fullscreen");
  const fs = await page.waitForFunction(() => document.fullscreenElement?.id === "game", null, { timeout: 10_000 }).then(() => true, () => false);
  check(fs, "fullscreen button puts the game view in fullscreen");
  if (fs) {
    const size = await page.evaluate(() => [document.getElementById("canvas").width, document.getElementById("canvas").height, screen.width, screen.height]);
    check(size[0] > 0 && size[1] > 0, "canvas resized in fullscreen", size.join(" x "));
    const label = await page.waitForFunction(() => document.getElementById("fullscreen").textContent === "Exit fullscreen", null, { timeout: 5_000 }).then(() => true, () => false);
    check(label, "button offers to exit", await page.locator("#fullscreen").textContent());
    await page.click("#fullscreen");
    const out = await page.waitForFunction(() => !document.fullscreenElement, null, { timeout: 10_000 }).then(() => true, () => false);
    check(out, "fullscreen button leaves fullscreen");
  }

  // Pointer lock on clicking the game; the capturing click is not passed to the game.
  const logLen = await page.evaluate(() => window.__shrek2.log.length);
  await page.mouse.click(640, 480);
  const locked = await page.waitForFunction(() => document.pointerLockElement?.id === "canvas", null, { timeout: 5_000 }).then(() => true, () => false);
  check(locked, "clicking the game locks the pointer");
  const clickLog = await page.evaluate((n) => window.__shrek2.log.slice(n).filter((l) => /input: event [45] /.test(l)), logLen);
  check(clickLog.length === 0, "capturing click is not sent to the game", clickLog.join(" | "));
  if (locked) {
    await page.mouse.click(640, 480);
    await page.waitForTimeout(500);
    const after = await page.evaluate((n) => window.__shrek2.log.slice(n).filter((l) => /input: event [45] /.test(l)), logLen);
    check(after.length === 2, "clicks while locked reach the game", after.join(" | "));
    await page.evaluate(() => document.exitPointerLock());
  }

  // The pause-key alias arrives as Escape (VK 0x1b, DIK 0x01) and produces no character.
  const n0 = await page.evaluate(() => window.__shrek2.log.length);
  await page.keyboard.press("Backquote");
  await page.waitForTimeout(500);
  const keyLog = await page.evaluate((n) => window.__shrek2.log.slice(n).filter((l) => l.includes("input:")), n0);
  check(
    keyLog.some((l) => /event 0 vk=0x1b scan=0x1 /.test(l)) && keyLog.some((l) => /event 1 vk=0x1b scan=0x1 /.test(l)) && !keyLog.some((l) => /event 2 /.test(l)),
    "` (Backquote) reaches the game as Esc",
    keyLog.join(" | "),
  );
}

if (!only || only === "view") {
  const engineLog = await page.evaluate(() => window.__shrek2.log);
  writeFileSync(join(tmp, "engine.log"), engineLog.join("\n"));
  const saveOpens = engineLog.filter((l) => /CreateFile\([^)]*Save[\\/]save\.bin/i.test(l));
  check(saveOpens.length > 0, "the game opens its save files", saveOpens.slice(0, 3).join(" | "));
}

if (!only || only === "help") {
  await page.goto(base);
  await waitState("ready");
  // Settings as the first version of the page wrote them: the Launcher's keys and mouse buttons.
  await page.evaluate(async () => {
    const key = "hkcu\\software\\activision\\shrek 2 team action\\settings";
    const manual = [0x11, 0x1f, 0x1e, 0x20, 0xc8, 0xd0, 0xcb, 0xcd, 0x39, 0x21, 0x1c, 0x2a, 0x01, 0x4f, 0x50, 0x0f];
    let text = `${key}\tresolution\tdword\t2\n${key}\tmousebindl\tdword\t6\n${key}\tmousebindr\tdword\t4\n${key}\tmousebindm\tdword\t5\n`;
    manual.forEach((k, a) => (text += `${key}\tk0_${a}\tdword\t${k}\n`));
    const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle("shrek2");
    const w = await (await dir.getFileHandle("registry.txt", { create: true })).createWritable();
    await w.write(text);
    await w.close();
    localStorage.removeItem("shrek2.controlsSeen");
  });
  await page.goto(`${base}?env=SHREK2_TRACE=input`);
  await waitState("ready");
  await page.click("#play");
  await page.locator("#help").waitFor({ state: "visible" });
  const rows = await page.locator("#help tbody tr").evaluateAll((trs) => trs.map((tr) => [...tr.children].map((td) => td.textContent).join(" | ")));
  const want = ["Move Up | ↑ | Left stick ↕", "Zoom In | Num 8 | Right stick ↕", "Cam Left | Num 4 | Right stick ↔", "Jump | Space | A", "Use | Z | B", "Attack | X | X", "Special | C | Y", "Start (pause) | Esc | Start", "Next Character | A | RB", "Previous Character | S | LB", "Display HUD | D | Back"];
  for (const w of want) check(rows.includes(w), `controls list: ${w}`);
  check((await page.locator("#help-close").innerText()) === "Play", "first start shows the controls with a Play button");
  const mouse = await page.locator("#help-mouse").innerText();
  check(/Wheel/.test(mouse) && !/Left button/.test(mouse), "mouse buttons unbound after migration", mouse.replace(/\n/g, " | "));
  const reg = await page.evaluate(async () => {
    const f = await (await (await (await navigator.storage.getDirectory()).getDirectoryHandle("shrek2")).getFileHandle("registry.txt")).getFile();
    return Object.fromEntries((await f.text()).split("\n").filter(Boolean).map((l) => l.split("\t")).map((p) => [p[1], p[3]]));
  });
  check(reg.k0_0 === String(0xc8) && reg.k0_9 === String(0x2c) && reg.mousebindl === "0" && reg.webdefaults === "2", "old page defaults migrated", `k0_0=${reg.k0_0} k0_9=${reg.k0_9} mousebindl=${reg.mousebindl}`);
  await page.click("#help-close");
  await waitState("running", 120_000);
  await page.waitForFunction(() => window.__shrek2.frames > 60, null, { timeout: 120_000 });
  const n0 = await page.evaluate(() => window.__shrek2.log.length);
  await page.keyboard.press("F1");
  await page.locator("#help").waitFor({ state: "visible" });
  check((await page.locator("#help-close").innerText()) === "Back to the game (F1)", "F1 opens the controls in game");
  await page.keyboard.press("KeyX"); // must not reach the game while the list is open
  await page.keyboard.press("F1");
  await page.locator("#help").waitFor({ state: "hidden" });
  await page.waitForTimeout(300);
  const events = await page.evaluate((n) => window.__shrek2.log.slice(n).filter((l) => l.includes("input: event")), n0);
  check(events.some((l) => /event 8 /.test(l)) && events.some((l) => /event 7 /.test(l)), "game paused (focus lost) while open, resumed after", events.join(" | "));
  check(!events.some((l) => /vk=0x58/.test(l)), "keys do not reach the game while the list is open");
  await page.mouse.move(600, 300);
  await page.mouse.move(640, 320);
  await page.click("#help-button");
  await page.locator("#help").waitFor({ state: "visible" });
  await page.keyboard.press("Escape");
  await page.locator("#help").waitFor({ state: "hidden" });
  check(true, "HUD Controls button opens the list; Esc closes it");
}

await ctx.close();
console.log(failures ? `${failures} check(s) failed` : "all checks passed");
process.exit(failures ? 1 : 0);
