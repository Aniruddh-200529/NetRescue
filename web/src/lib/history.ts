// Experiment history kept in the browser's IndexedDB. Entries are the unmodified engine results.
import type { ExperimentResult } from '../engine/types';

export interface HistoryEntry { id: string; savedAt: string; title: string; result: ExperimentResult; csv: string }

const DB = 'netrescue', STORE = 'experiments';

function open(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE, { keyPath: 'id' });
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function tx<T>(mode: IDBTransactionMode, work: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const db = await open();
  return new Promise((resolve, reject) => {
    const request = work(db.transaction(STORE, mode).objectStore(STORE));
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

export async function saveHistory(entry: HistoryEntry): Promise<void> { await tx('readwrite', (s) => s.put(entry)); }
export async function deleteHistory(id: string): Promise<void> { await tx('readwrite', (s) => s.delete(id)); }
export async function listHistory(): Promise<HistoryEntry[]> {
  const all = await tx<HistoryEntry[]>('readonly', (s) => s.getAll() as IDBRequest<HistoryEntry[]>);
  return all.sort((a, b) => b.savedAt.localeCompare(a.savedAt));
}

export function experimentTitle(result: ExperimentResult): string {
  const sweep = result.spec.sweep.parameter !== 'none' ? ` · sweep ${result.spec.sweep.parameter}` : '';
  return `${result.spec.protocols.length} protocol(s) · ${result.spec.trials} trial(s)${sweep} · ${result.topology.description}`;
}

/** Selected entry handed from the History page to the Comparison page. */
let pending: HistoryEntry | null = null;
export function setPendingEntry(entry: HistoryEntry | null) { pending = entry; }
export function takePendingEntry(): HistoryEntry | null { const e = pending; pending = null; return e; }
