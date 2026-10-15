import { createReadStream, existsSync, statSync } from "node:fs";
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
 * Development only: serves the local engine build (`/engine/*` from work/build-web) and the
 * developer's own disc image (`/dev-iso`). Neither is part of `vite build` output, so a
 * deployable dist/ never contains game data or translated game code.
 */
function devFiles(): Plugin {
  return {
    name: "shrek2-dev-files",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const url = (req.url ?? "").split("?")[0];
        let file: string | null = null;
        if (url.startsWith("/engine/")) file = join(root, "work/build-web", url.slice("/engine/".length));
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
  plugins: [devFiles()],
  server: { headers: isolation, port: 5173, strictPort: true, fs: { allow: [root] } },
  preview: { headers: isolation },
  worker: { format: "es" },
  build: { target: "es2022", outDir: "dist", emptyOutDir: true },
});
