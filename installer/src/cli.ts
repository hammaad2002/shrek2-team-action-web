/**
 * Developer CLI: install the game from a disc image into a directory.
 *   node installer/src/cli.ts <disc.iso> <outdir> [--no-verify] [--only <prefix>]
 */
import { install } from "./install.ts";
import { DirSink, NodeFileSource } from "./node.ts";

const args = process.argv.slice(2);
const positional = args.filter((a, i) => !a.startsWith("--") && args[i - 1] !== "--only");
const only = args.includes("--only") ? args[args.indexOf("--only") + 1] : undefined;
if (positional.length !== 2) {
  console.error("usage: node installer/src/cli.ts <disc.iso> <outdir> [--no-verify] [--only <prefix>]");
  process.exit(2);
}
const [isoPath, outDir] = positional;
const src = new NodeFileSource(isoPath);
const t0 = performance.now();
let last = 0;
const report = await install(src, new DirSink(outDir), {
  verify: !args.includes("--no-verify"),
  include: only ? (p) => p.toLowerCase().startsWith(only.toLowerCase()) : undefined,
  onProgress: (p) => {
    const now = performance.now();
    if (now - last > 1000 || p.phase === "done") {
      last = now;
      process.stderr.write(`\r${p.phase} ${p.done}/${p.total} ${p.path ?? ""}`.padEnd(100).slice(0, 100));
    }
  },
});
src.close();
const secs = ((performance.now() - t0) / 1000).toFixed(1);
console.error(`\nvolume ${report.volumeId}: wrote ${report.written} files (${(report.bytes / 1e6).toFixed(1)} MB) in ${secs}s; ` +
  `MD5-verified ${report.verified}; skipped ${report.skipped}`);
if (report.mismatches.length) {
  console.error(`${report.mismatches.length} problems:\n  ${report.mismatches.slice(0, 20).join("\n  ")}`);
  process.exit(1);
}
