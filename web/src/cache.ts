/** OPFS locations shared by the page and its workers. */
import { TRANSLATOR_VERSION } from "../../recomp/src/version.ts";

export const INSTALL_DIR = "shrek2";

/** Cached in-browser translation of a given Shrek 2.exe (by SHA-256) for this translator version. */
export function cacheName(sha: string): string {
  return `game-${sha.slice(0, 16)}-t${TRANSLATOR_VERSION}.wasm`;
}

export async function sha256(bytes: BufferSource): Promise<string> {
  return [...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))].map((x) => x.toString(16).padStart(2, "0")).join("");
}
