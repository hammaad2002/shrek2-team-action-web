/**
 * Fullscreen for the game view. In Chromium browsers the Keyboard Lock API lets Escape reach the
 * game while fullscreen (holding Escape still exits); elsewhere Escape leaves fullscreen and the
 * pause-key alias (settings) stands in for it.
 */
type KeyboardLock = { lock(keys?: string[]): Promise<void>; unlock(): void };

const keyboard = (navigator as Navigator & { keyboard?: KeyboardLock }).keyboard;

export const canLockEscape = typeof keyboard?.lock === "function";

export function isFullscreen(): boolean {
  return document.fullscreenElement != null;
}

export async function enterFullscreen(el: HTMLElement): Promise<void> {
  await el.requestFullscreen({ navigationUI: "hide" });
  if (canLockEscape) {
    try {
      await keyboard!.lock(["Escape"]);
    } catch {
      // Not fatal: Escape then simply leaves fullscreen.
    }
  }
}

export async function exitFullscreen(): Promise<void> {
  if (canLockEscape) keyboard!.unlock();
  if (document.fullscreenElement) await document.exitFullscreen();
}

/** Keep the keyboard lock in step with fullscreen changes made by the browser (e.g. hold-Esc). */
export function watchFullscreen(onChange: (fullscreen: boolean) => void): void {
  document.addEventListener("fullscreenchange", () => {
    if (!document.fullscreenElement && canLockEscape) keyboard!.unlock();
    onChange(isFullscreen());
  });
}
