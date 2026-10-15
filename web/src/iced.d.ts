/// <reference types="vite/client" />
// The browser build of iced-x86 (icedEsm() in vite.config.ts) adds an explicit initialiser.
export {};
declare module "iced-x86" {
  export function initIced(bytes: BufferSource): void;
}
