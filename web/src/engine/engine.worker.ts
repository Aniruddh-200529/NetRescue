/// <reference lib="webworker" />
// Web Worker that hosts the C engine so long experiments never block the UI thread.
// It only forwards JSON strings to nr_api_call; it contains no simulation logic.
import wasmUrl from './generated/netrescue_engine.wasm?url';
import { loadRawEngine } from './raw';

const ready = loadRawEngine({ locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path) });

self.onmessage = async (event: MessageEvent<{ id: number; request: string }>) => {
  const { id, request } = event.data;
  try {
    const call = await ready;
    self.postMessage({ id, response: call(request) });
  } catch (error) {
    self.postMessage({ id, response: JSON.stringify({ ok: false, api_version: 1, error: `engine failed to load: ${String(error)}` }) });
  }
};
