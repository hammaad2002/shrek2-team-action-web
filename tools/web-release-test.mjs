#!/usr/bin/env node
// End-to-end check of a deployed (or `vite preview`ed) production build, as a first-time player:
// fresh browser profile, disc image chosen through the page's file picker, install, the controls
// list on first Play, translation, and play to the main menu.
//   node tools/web-release-test.mjs <url> [--iso SHRK2TMA_1.iso] [--saves backup.zip] [--browser chrome|firefox|webkit] [--frames 3500]
import { rmSync, writeFileSync } from "node:fs";
import { browserArgs, launch } from "./browsers.mjs";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
};
const url = args[0];
if (!url || url.startsWith("--")) {
  console.error("usage: node tools/web-release-test.mjs <url> [--iso path] [--browser chrome|firefox|webkit]");
  process.exit(2);
}
const iso = opt("iso", "SHRK2TMA_1.iso");
const want = Number(opt("frames", "3500"));
const saves = opt("saves", "");
const { browser } = browserArgs(args);
const profile = `work/${browser}-profile-release-test`;
rmSync(profile, { recursive: true, force: true });

let failures = 0;
const check = (ok, what, detail = "") => {
  console.log(`${ok ? "PASS" : "FAIL"} ${what}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures++;
};
const t0 = Date.now();
const secs = () => ((Date.now() - t0) / 1000).toFixed(0);
const ctx = await launch({ ...browserArgs(args), profile });
const page = ctx.pages()[0] ?? (await ctx.newPage());
const errors = [];
page.on("console", (m) => m.type() === "error" && errors.push(m.text()));
page.on("pageerror", (e) => errors.push(e.message));
const response = await page.goto(url);
const headers = response.headers();
check(headers["cross-origin-opener-policy"] === "same-origin" && headers["cross-origin-embedder-policy"] === "require-corp", "cross-origin isolation headers", `${headers["cross-origin-opener-policy"]} / ${headers["cross-origin-embedder-policy"]}`);
await page.waitForFunction(() => window.__shrek2?.state === "need-install", null, { timeout: 60_000 });
check(await page.evaluate(() => crossOriginIsolated), "page is cross-origin isolated");
check(/Not affiliated/.test(await page.locator("footer").innerText()), "notice shown", await page.locator("footer").innerText());

await page.setInputFiles("#iso", iso);
await page.waitForFunction(() => ["ready", "error"].includes(window.__shrek2.state), null, { timeout: 900_000, polling: 2000 });
check((await page.evaluate(() => window.__shrek2.state)) === "ready", "installed from the chosen disc image", `${secs()} s; ${await page.locator("#log").innerText()}`);

if (saves) {
  await page.setInputFiles("#save-import", saves);
  await page.waitForFunction(() => /Restored|Nothing restored|failed/.test(document.getElementById("save-status").textContent), null, { timeout: 30_000 });
  check(/^Restored/.test(await page.locator("#save-status").innerText()), "save backup restored into the new install", await page.locator("#save-status").innerText());
}

await page.click("#play");
await page.locator("#help").waitFor({ state: "visible" });
check((await page.locator("#help-close").innerText()) === "Play", "controls list before the first start");
await page.click("#help-close");
await page.waitForFunction((n) => window.__shrek2.state === "error" || window.__shrek2.frames >= n, want, { timeout: 900_000, polling: 2000 });
const s = await page.evaluate(() => ({ state: window.__shrek2.state, frames: window.__shrek2.frames, log: document.getElementById("log").innerText }));
check(s.state === "running" && s.frames >= want, `translated and ran to frame ${want}`, `${secs()} s; ${s.log.split("\n").filter(Boolean).pop() ?? ""}`);
await page.screenshot({ path: `work/release-test-${browser}.png` });
const engineLog = await page.evaluate(() => window.__shrek2.log.join("\n"));
writeFileSync(`work/release-test-${browser}.log`, engineLog);
check(!/\[(fatal|error)\]/.test(engineLog), "no engine errors", (engineLog.match(/.*\[(fatal|error)\].*/g) ?? []).slice(0, 3).join(" | "));
check(errors.length === 0, "no console errors", errors.slice(0, 3).join(" | "));
await ctx.close();
rmSync(profile, { recursive: true, force: true });
console.log(failures ? `${failures} check(s) failed` : "all checks passed");
process.exit(failures ? 1 : 0);
