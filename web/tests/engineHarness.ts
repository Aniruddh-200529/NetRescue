// Loads the real C engine (WebAssembly build) inside the test process.
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { loadRawEngine, type RawCall } from '../src/engine/raw';
import { EngineClient } from '../src/engine/client';

const wasmPath = resolve(process.cwd(), 'src/engine/generated/netrescue_engine.wasm');

export async function loadTestEngine(): Promise<{ raw: RawCall; client: EngineClient }> {
  const raw = await loadRawEngine({ wasmBinary: readFileSync(wasmPath), locateFile: (path) => (path.endsWith('.wasm') ? wasmPath : path) });
  return { raw, client: new EngineClient(async (request) => raw(request)) };
}
