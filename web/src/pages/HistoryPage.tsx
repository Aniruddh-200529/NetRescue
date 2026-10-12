import { useEffect, useState } from 'react';
import type { ExperimentResult } from '../engine/types';
import { deleteHistory, experimentTitle, listHistory, saveHistory, setPendingEntry, type HistoryEntry } from '../lib/history';
import { downloadText, readFileText } from '../lib/download';
import { PROTOCOL_LABEL } from '../lib/format';
import { useLab } from '../state/labStore';

export function HistoryPage({ onOpen }: { onOpen: () => void }) {
  const [entries, setEntries] = useState<HistoryEntry[] | null>(null);
  const notify = useLab((s) => s.notify);
  const refresh = () => listHistory().then(setEntries, () => setEntries([]));
  useEffect(() => { void refresh(); }, []);

  const importJson = async () => {
    const text = await readFileText('.json');
    if (!text) return;
    try {
      const result = JSON.parse(text) as ExperimentResult;
      if (!result.spec || !Array.isArray(result.runs) || !Array.isArray(result.aggregates)) throw new Error('not a NetRescue experiment export');
      await saveHistory({ id: crypto.randomUUID(), savedAt: new Date().toISOString(), title: `Imported: ${experimentTitle(result)}`, result, csv: '' });
      notify('info', 'Experiment imported'); void refresh();
    } catch (e) { notify('error', `Import failed: ${(e as Error).message}`); }
  };

  return (
    <div className="mode-b">
      <div className="mode-banner"><div><h2>Experiment history</h2><div className="small secondary">Every comparison you run is stored in this browser (IndexedDB) exactly as the engine returned it.</div></div>
        <div className="btn-row" style={{ marginLeft: 'auto' }}><button className="btn" onClick={() => void importJson()}>Import JSON export</button></div></div>
      <section className="panel">
        {entries === null ? <div className="empty">Loading…</div> : entries.length === 0 ? <div className="empty">No experiments yet. Run one in Protocol Comparison.</div> : (
          <div className="table-wrap">
            <table className="data">
              <thead><tr><th>Saved</th><th>Experiment</th><th>Protocols</th><th className="num">Runs</th><th></th></tr></thead>
              <tbody>
                {entries.map((e) => (
                  <tr key={e.id}>
                    <td className="mono">{new Date(e.savedAt).toLocaleString()}</td>
                    <td>{e.title}</td>
                    <td>{e.result.spec.protocols.map((p) => PROTOCOL_LABEL[p]).join(', ')}</td>
                    <td className="num">{e.result.runs.length}</td>
                    <td><div className="btn-row">
                      <button className="btn sm violet" onClick={() => { setPendingEntry(e); onOpen(); }}>Open</button>
                      <button className="btn sm" onClick={() => downloadText(`netrescue-experiment-${e.id.slice(0, 8)}.json`, JSON.stringify(e.result, null, 2), 'application/json')}>JSON</button>
                      {e.csv && <button className="btn sm" onClick={() => downloadText(`netrescue-experiment-${e.id.slice(0, 8)}.csv`, e.csv, 'text/csv')}>CSV</button>}
                      <button className="btn sm ghost" onClick={() => void deleteHistory(e.id).then(refresh)}>Delete</button>
                    </div></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </div>
  );
}
