// SPDX-License-Identifier: AGPL-3.0-or-later
import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

// Development: the API goes to the simulator (build/doseph_sim_server --port 8080).
const target = process.env.DP_SIM ?? "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [preact()],
  build: {
    target: "es2020",
    outDir: "dist",
    assetsInlineLimit: 0,
    cssCodeSplit: false,
    sourcemap: false,
    // One chunk that preloads nothing: no polyfill for it in the app
    modulePreload: { polyfill: false },
  },
  server: {
    // The host stays localhost:5173: the simulator's origin check applies here too
    proxy: { "/api": { target, changeOrigin: false } },
  },
});
