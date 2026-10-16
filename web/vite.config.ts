import { createReadStream, existsSync, readFileSync, statSync } from "node:fs";
import { extname, join, resolve } from "node:path";
import { defineConfig, type Plugin } from "vite";

// Cross-origin isolation: required for SharedArrayBuffer (the engine's threads share memory).
const isolation = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
};

const root = resolve(import.meta.dirname, "..");
const types: Record<string, string> = { ".js": "text/javascript", ".wasm": "application/wasm", ".iso": "application/octet-stream" };

/**
 * iced-x86 ships a Node-only (CommonJS + fs) wasm-bindgen build; expose it to the browser as an
 * ES module whose wasm is instantiated explicitly with `initIced(bytes)`.
 */
function icedEsm(): Plugin {
  const VIRTUAL = "\0iced-x86-esm";
  return {
    name: "iced-x86-esm",
    enforce: "pre",
    resolveId: (id) => (id === "iced-x86" ? VIRTUAL : null),
    load(id) {
      if (id !== VIRTUAL) return null;
      let src = readFileSync(join(root, "node_modules/iced-x86/iced_x86.js"), "utf8");
      src = src.replace("const { TextDecoder, TextEncoder } = require(`util`);", "");
      src = src.slice(0, src.indexOf("const path = require('path')"));
      const names = [...src.matchAll(/^module\.exports\.([A-Za-z_]\w*) = /gm)].map((m) => m[1]).filter((n) => !n.startsWith("__"));
      return [
        "const module = { exports: {} };",
        src,
        "export function initIced(bytes) {",
        "  wasm = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports).exports;",
        "  module.exports.__wasm = wasm;",
        "}",
        // Aliases avoid clashing with the classes the CommonJS source declares at top level.
        ...names.map((n) => `const __iced_${n} = module.exports.${n};`),
        `export { ${names.map((n) => `__iced_${n} as ${n}`).join(", ")} };`,
      ].join("\n");
    },
  };
}

/**
 * Development only: serves the local C-backend engine build (`/dev-engine/*` from work/build-web,
 * which contains translated game code) and the developer's own disc image (`/dev-iso`). Neither
 * is part of `vite build` output, so a deployable dist/ never contains game data or game code.
 */
function devFiles(): Plugin {
  return {
    name: "shrek2-dev-files",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const url = (req.url ?? "").split("?")[0];
        let file: string | null = null;
        if (url.startsWith("/dev-engine/")) file = join(root, "work/build-web", url.slice("/dev-engine/".length));
        else if (url === "/dev-iso") file = process.env.SHREK2_ISO ?? join(root, "SHRK2TMA_1.iso");
        if (!file || !existsSync(file) || !statSync(file).isFile()) return next();
        res.setHeader("Content-Type", types[extname(file)] ?? "application/octet-stream");
        res.setHeader("Content-Length", String(statSync(file).size));
        res.setHeader("Cache-Control", "no-store");
        for (const [k, v] of Object.entries(isolation)) res.setHeader(k, v);
        res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
        createReadStream(file).pipe(res);
      });
    },
  };
}

export default defineConfig({
  root: import.meta.dirname,
  plugins: [icedEsm(), devFiles()],
  server: { headers: isolation, port: 5173, strictPort: true, fs: { allow: [root] } },
  preview: { headers: isolation },
  // Worker bundles do not inherit `plugins`: the translator worker needs the iced-x86 shim too.
  worker: { format: "es", plugins: () => [icedEsm()] },
  build: { target: "es2022", outDir: "dist", emptyOutDir: true },
});
