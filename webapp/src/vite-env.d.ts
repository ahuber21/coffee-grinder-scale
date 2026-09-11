/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_DEVICE_HOST?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
