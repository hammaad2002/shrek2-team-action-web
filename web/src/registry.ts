/**
 * The engine's virtual registry file (OPFS shrek2/registry.txt; see runtime/src/hle/advapi32.cpp):
 * one `key<TAB>name<TAB>dword|sz<TAB>value` line per value, keys and names in lower case. The game
 * only reads it, so the page may rewrite it whenever the game is not running.
 */
import { INSTALL_DIR } from "./cache.ts";
import { readOpfsFile, writeOpfsFile } from "./opfs-write.ts";

export const SETTINGS_KEY = "hkcu\\software\\activision\\shrek 2 team action\\settings";

export type RegValue = { type: "dword"; value: number } | { type: "sz"; value: string };

export class Registry {
  private readonly keys = new Map<string, Map<string, RegValue>>();

  static parse(text: string): Registry {
    const r = new Registry();
    for (const line of text.split("\n")) {
      const [key, name, type, ...rest] = line.replace(/\r$/, "").split("\t");
      const value = rest.join("\t");
      if (!key || name === undefined || !type) continue;
      if (type === "dword") r.set(key, name, { type, value: Number(value) >>> 0 });
      else if (type === "sz") r.set(key, name, { type, value });
    }
    return r;
  }

  serialize(): string {
    let out = "";
    for (const [key, values] of this.keys)
      for (const [name, v] of values) out += `${key}\t${name}\t${v.type}\t${v.type === "dword" ? v.value >>> 0 : v.value}\n`;
    return out;
  }

  get(key: string, name: string): RegValue | undefined {
    return this.keys.get(key.toLowerCase())?.get(name.toLowerCase());
  }

  dword(key: string, name: string): number | undefined {
    const v = this.get(key, name);
    return v?.type === "dword" ? v.value : undefined;
  }

  sz(key: string, name: string): string | undefined {
    const v = this.get(key, name);
    return v?.type === "sz" ? v.value : undefined;
  }

  set(key: string, name: string, v: RegValue): void {
    const k = key.toLowerCase();
    if (!this.keys.has(k)) this.keys.set(k, new Map());
    this.keys.get(k)!.set(name.toLowerCase(), v.type === "dword" ? { type: "dword", value: v.value >>> 0 } : v);
  }

  delete(key: string, name: string): void {
    this.keys.get(key.toLowerCase())?.delete(name.toLowerCase());
  }
}

const PATH = [INSTALL_DIR, "registry.txt"];

export async function loadRegistry(): Promise<Registry> {
  const data = await readOpfsFile(PATH);
  return Registry.parse(data ? new TextDecoder().decode(data) : "");
}

export async function saveRegistry(r: Registry): Promise<void> {
  await writeOpfsFile(PATH, new TextEncoder().encode(r.serialize()));
}
