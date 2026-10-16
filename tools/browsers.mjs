// Browser launcher shared by the web test tools (local development only).
//   chrome:  the system Chrome (--angle vulkan|gl|swiftshader picks the ANGLE backend)
//   firefox, webkit: Playwright's builds in ~/.cache/ms-playwright
//            (`npx playwright-core install firefox webkit`; WebKit on Ubuntu 22.04 also needs
//            libavif13, libgstreamer-plugins-bad1.0-0 and their dependencies copied into
//            minibrowser-wpe/sys/lib, see docs/JOURNAL.md)
// Each browser and port keeps a persistent profile in work/<browser>-profile-<port>, so the
// installed game (OPFS) survives between runs; Chrome's is work/chrome-profile-<port>.
import { chromium, firefox, webkit } from "playwright-core";

export function browserArgs(args) {
  const opt = (name, dflt) => {
    const i = args.indexOf(`--${name}`);
    return i >= 0 ? args[i + 1] : dflt;
  };
  return { browser: opt("browser", "chrome"), angle: opt("angle", "vulkan"), headed: args.includes("--headed") };
}

export async function launch({ browser = "chrome", angle = "vulkan", headed = false, port = "5174", profile = "", extra = {} }) {
  const common = { headless: !headed, viewport: { width: 1280, height: 960 }, ...extra };
  const dir = profile || (browser === "chrome" ? `work/chrome-profile-${port}` : `work/${browser}-profile-${port}`);
  if (browser === "chrome") {
    return chromium.launchPersistentContext(dir, {
      ...common,
      executablePath: "/usr/bin/google-chrome",
      args: [
        "--autoplay-policy=no-user-gesture-required",
        "--enable-gpu",
        "--ignore-gpu-blocklist",
        `--use-angle=${angle}`,
        ...(angle === "vulkan" ? ["--enable-features=Vulkan"] : []),
        "--enable-unsafe-swiftshader",
      ],
    });
  }
  if (browser === "firefox") {
    return firefox.launchPersistentContext(dir, {
      ...common,
      firefoxUserPrefs: { "media.autoplay.default": 0, "webgl.force-enabled": true },
    });
  }
  if (browser === "webkit") return webkit.launchPersistentContext(dir, common);
  throw new Error(`unknown --browser ${browser}`);
}
