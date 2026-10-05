/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_DEVICE_HOST?: string;
  readonly VITE_POSTGREST_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
