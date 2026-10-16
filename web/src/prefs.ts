/**
 * Browser-only preferences (the game's own settings live in its registry file). Kept in this
 * site's localStorage; every access tolerates storage being unavailable.
 */
export interface Prefs {
  /** KeyboardEvent.code that the game also receives as Escape. */
  pauseAlias: string;
}

const KEY = "shrek2.prefs";
export const DEFAULT_PREFS: Prefs = { pauseAlias: "Backquote" };

export function loadPrefs(): Prefs {
  try {
    return { ...DEFAULT_PREFS, ...(JSON.parse(localStorage.getItem(KEY) ?? "{}") as Partial<Prefs>) };
  } catch {
    return { ...DEFAULT_PREFS };
  }
}

export function savePrefs(p: Prefs): void {
  try {
    localStorage.setItem(KEY, JSON.stringify(p));
  } catch {
    // Private windows may refuse storage; the defaults apply next time.
  }
}
