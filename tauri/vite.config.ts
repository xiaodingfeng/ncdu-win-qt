import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Tauri expects a fixed dev port and leaves the console for its own output.
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  build: {
    target: "chrome105",
    minify: "esbuild",
    cssMinify: true,
    sourcemap: false,
  },
  server: {
    port: 1420,
    strictPort: true,
    watch: {
      ignored: ["**/src-tauri/**"],
    },
  },
});
