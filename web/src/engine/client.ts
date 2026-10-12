// Promise-based client for the C engine API. Every call is a JSON request {cmd, ...params}
// answered by src/api.c; the response's "result" is returned as-is.
import type { ApiResponse } from './types';

export type Transport = (requestJson: string) => Promise<string>;

export class EngineError extends Error {}

export class EngineClient {
  private queue: Promise<unknown> = Promise.resolve();
  constructor(private readonly transport: Transport) {}

  /** Sends one command. Calls are serialised so the engine sees them in order. */
  call<T>(cmd: string, params: Record<string, unknown> = {}): Promise<T> {
    const run = async () => {
      const text = await this.transport(JSON.stringify({ cmd, ...params }));
      const response = JSON.parse(text) as ApiResponse<T>;
      if (!response.ok) throw new EngineError(response.error ?? 'engine error');
      return response.result as T;
    };
    const next = this.queue.then(run, run);
    this.queue = next.catch(() => undefined);
    return next;
  }
}

/** Transport that talks to the engine running inside a dedicated Web Worker. */
export function workerTransport(): Transport {
  const worker = new Worker(new URL('./engine.worker.ts', import.meta.url), { type: 'module' });
  let nextId = 1;
  const pending = new Map<number, (response: string) => void>();
  worker.onmessage = (event: MessageEvent<{ id: number; response: string }>) => {
    pending.get(event.data.id)?.(event.data.response);
    pending.delete(event.data.id);
  };
  return (request) => new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    worker.postMessage({ id, request });
  });
}
