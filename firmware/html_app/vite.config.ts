import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': 'http://matter-controller.local',
      '/ws': {
        target: 'ws://matter-controller.local',
        ws: true,
      },
    },
  },
  build: {
    rollupOptions: {
      output: {
        dir: '../html_compiled_app',
        entryFileNames: 'app.js',
        assetFileNames: 'app.css',
        chunkFileNames: 'chunk.js',
        manualChunks: undefined,
      }
    }
  }
})
