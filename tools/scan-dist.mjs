#!/usr/bin/env node
// Release guard: finds byte sequences that a deployable folder shares with the game's files.
//   node tools/scan-dist.mjs <folder> <game-dir-or-file>... [--allow <file>]... [--window 48] [--report 20]
// Every file under <folder> is indexed by a rolling hash of each `window`-byte span. Spans that
// carry no information are skipped: fewer than 12 distinct byte values (padding, runs) or an
// arithmetic sequence of 1–4-byte numbers (counters and index tables, which installer databases
// and lookup tables share by coincidence). Every game file is hashed the same way, candidates are
// compared byte for byte and shared runs grown to full length. A run that also occurs in an
// --allow file (third-party code shipped as is, e.g. FFmpeg's libavcodec with its Bink tables)
// is listed but accepted. Exit status 1 when any other shared run is found.
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  if (i < 0) return dflt;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
};
const allow = [];
for (let v; (v = opt("allow", null)) !== null; ) allow.push(v);
const W = Number(opt("window", "48"));
const REPORT = Number(opt("report", "20"));
const [dist, ...gameRoots] = args;
if (!dist || !gameRoots.length) {
  console.error("usage: node tools/scan-dist.mjs <folder> <game-dir-or-file>... [--window 48]");
  process.exit(2);
}

const files = (root) => {
  if (statSync(root).isFile()) return [root];
  const out = [];
  for (const e of readdirSync(root, { withFileTypes: true })) {
    const p = join(root, e.name);
    if (e.isDirectory()) out.push(...files(p));
    else if (e.isFile()) out.push(p);
  }
  return out;
};

// Polynomial rolling hash mod 2^32: h = sum b[i] * B^(W-1-i).
const B = 0x01000193;
let BW = 1;
for (let i = 0; i < W; i++) BW = Math.imul(BW, B);
// An arithmetic sequence of k-byte numbers has periodic byte differences at distance k.
const arithmetic = (buf, at) => {
  for (let k = 1; k <= 4; k++) {
    let periodic = true;
    for (let i = at; i + 2 * k < at + W && periodic; i++) {
      periodic = ((buf[i + k] - buf[i]) & 0xff) === ((buf[i + 2 * k] - buf[i + k]) & 0xff);
    }
    if (periodic) return true;
  }
  return false;
};
const interesting = (buf, at) => {
  const seen = new Uint8Array(256);
  let distinct = 0;
  for (let i = at; i < at + W; i++) if (!seen[buf[i]]++ && ++distinct >= 12) return !arithmetic(buf, at);
  return false;
};
// Calls fn(hash, offset) for every W-byte window of buf.
function eachWindow(buf, fn) {
  if (buf.length < W) return;
  let h = 0;
  for (let i = 0; i < W; i++) h = (Math.imul(h, B) + buf[i]) >>> 0;
  const last = buf.length - W;
  for (let at = 0; ; at++) {
    fn(h, at);
    if (at === last) return;
    h = (Math.imul(h, B) - Math.imul(buf[at], BW) + buf[at + W]) >>> 0;
  }
}

// Bitmap of indexed hashes (2^28 bits) to skip most Map lookups while scanning game files.
const bits = new Uint32Array(1 << 23);
const mark = (h) => (bits[(h >>> 4) & 0x7fffff] |= 1 << (h & 31));
const marked = (h) => (bits[(h >>> 4) & 0x7fffff] & (1 << (h & 31))) !== 0;

const distFiles = files(dist).map((p) => ({ path: relative(dist, p), data: readFileSync(p) }));
const index = new Map(); // hash -> [[fileIndex, offset], ...]
let indexed = 0;
distFiles.forEach((f, fi) =>
  eachWindow(f.data, (h, at) => {
    if (!interesting(f.data, at)) return;
    let list = index.get(h);
    if (!list) index.set(h, (list = []));
    if (list.length < 8) list.push([fi, at]);
    mark(h);
    indexed++;
  }),
);
console.log(`indexed ${indexed} windows of ${W} bytes in ${distFiles.length} files under ${dist}`);

const hits = new Map(); // "distFile:start" -> {dist, dstart, len, game, gstart}
let scanned = 0;
for (const root of gameRoots) {
  for (const path of files(root)) {
    const g = readFileSync(path);
    scanned += g.length;
    eachWindow(g, (h, at) => {
      if (!marked(h)) return;
      const list = index.get(h);
      if (!list) return;
      for (const [fi, off] of list) {
        const d = distFiles[fi].data;
        if (d.compare(g, at, at + W, off, off + W) !== 0) continue;
        // Grow the shared run in both directions.
        let s = 0;
        while (off - s > 0 && at - s > 0 && d[off - s - 1] === g[at - s - 1]) s++;
        let e = W;
        while (off + e < d.length && at + e < g.length && d[off + e] === g[at + e]) e++;
        const key = `${fi}:${off - s}`;
        const prev = hits.get(key);
        if (!prev || prev.len < s + e) hits.set(key, { dist: distFiles[fi].path, dstart: off - s, len: s + e, game: path, gstart: at - s });
      }
    });
  }
}
console.log(`scanned ${(scanned / 1048576).toFixed(0)} MB of game files`);
const found = [...hits.values()].sort((a, b) => b.len - a.len);
// Overlapping reports of one run collapse to the longest.
const runs = [];
for (const h of found) if (!runs.some((r) => r.dist === h.dist && h.dstart < r.dstart + r.len && r.dstart < h.dstart + h.len)) runs.push(h);
const allowed = allow.map((p) => ({ path: p, data: readFileSync(p) }));
const bytesOf = (r) => distFiles.find((f) => f.path === r.dist).data.subarray(r.dstart, r.dstart + r.len);
// A run is explained by a third-party file when its core (ignoring up to 16 bytes at each end,
// where neighbouring bytes may match by chance) occurs there.
for (const r of runs) {
  const trim = Math.max(0, Math.min(16, Math.floor((r.len - W) / 2)));
  const core = bytesOf(r).subarray(trim, r.len - trim);
  r.allowedBy = allowed.find((a) => a.data.indexOf(core) >= 0)?.path;
}
const bad = runs.filter((r) => !r.allowedBy);
const show = (r) => {
  const head = bytesOf(r).subarray(0, 32).toString("hex");
  return `  ${r.len} bytes: ${r.dist} @${r.dstart}  ==  ${r.game} @${r.gstart}  [${head}${r.len > 32 ? "…" : ""}]`;
};
for (const r of runs.filter((x) => x.allowedBy)) console.log(`${show(r)}\n      accepted: also in ${r.allowedBy}`);
if (!bad.length) {
  console.log(runs.length ? "no shared byte runs besides accepted third-party ones" : "no shared byte runs");
  process.exit(0);
}
console.log(`${bad.length} shared byte run(s):`);
for (const r of bad.slice(0, REPORT)) console.log(show(r));
process.exit(1);
