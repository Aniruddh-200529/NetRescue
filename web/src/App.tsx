import { useEffect, useState } from 'react';
import type { EngineClient } from './engine/client';
import { useLab } from './state/labStore';
import { SimulationLab } from './pages/lab/SimulationLab';
import { ComparisonLab } from './pages/compare/ComparisonLab';
import { HistoryPage } from './pages/HistoryPage';
import { HelpPage } from './pages/HelpPage';

export type Page = 'lab' | 'compare' | 'history' | 'help';
const PAGES: { id: Page; label: string }[] = [
  { id: 'lab', label: 'Network Simulation' },
  { id: 'compare', label: 'Protocol Comparison' },
  { id: 'history', label: 'Experiment History' },
  { id: 'help', label: 'Help / About' },
];

function pageFromHash(): Page {
  const id = window.location.hash.replace('#/', '') as Page;
  return PAGES.some((p) => p.id === id) ? id : 'lab';
}

export function App({ engine }: { engine: EngineClient }) {
  const [page, setPage] = useState<Page>(pageFromHash);
  const { version, engineError, message, init } = useLab();

  useEffect(() => { void init(engine); }, [engine, init]);
  useEffect(() => {
    const onHash = () => setPage(pageFromHash());
    window.addEventListener('hashchange', onHash);
    return () => window.removeEventListener('hashchange', onHash);
  }, []);
  useEffect(() => {
    if (!message) return;
    const timer = setTimeout(() => useLab.setState({ message: null }), message.kind === 'error' ? 7000 : 3500);
    return () => clearTimeout(timer);
  }, [message]);

  const go = (id: Page) => { window.location.hash = `#/${id}`; setPage(id); };

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <div className="brand-mark" aria-hidden>
            <svg width="20" height="20" viewBox="0 0 32 32"><circle cx="8" cy="16" r="4" fill="#3987e5" /><circle cx="24" cy="8" r="4" fill="#199e70" /><circle cx="24" cy="24" r="4" fill="#d95926" /><path d="M8 16L24 8M8 16L24 24" stroke="#c8d3e0" strokeWidth="2" /></svg>
          </div>
          <div><div className="brand-name">NetRescue</div><div className="brand-sub">Network Reliability &amp; Simulation Lab</div></div>
        </div>
        <nav className="nav" aria-label="Modes">
          {PAGES.map((p) => (
            <button key={p.id} className={`${page === p.id ? 'active' : ''} ${p.id === 'compare' ? 'mode-b' : ''}`} onClick={() => go(p.id)}>{p.label}</button>
          ))}
        </nav>
        <div className="engine-badge" title={version ? `${version.engine}, API v${version.api_version}, built ${version.built}` : ''}>
          <span className="dot" style={{ background: engineError ? 'var(--critical)' : version ? 'var(--good)' : 'var(--warning)' }} />
          {engineError ? 'C engine failed to load' : version ? `C engine · ${version.target} · API v${version.api_version}` : 'Loading C engine…'}
        </div>
      </header>
      <main className="main">
        {engineError && <div className="panel panel-body" style={{ borderColor: 'var(--critical)' }}>The WebAssembly engine could not be loaded: {engineError}</div>}
        {!version && !engineError && <div className="empty">Loading the C simulation engine (WebAssembly)…</div>}
        {version && page === 'lab' && <SimulationLab />}
        {version && page === 'compare' && <ComparisonLab engine={engine} />}
        {version && page === 'history' && <HistoryPage onOpen={() => go('compare')} />}
        {version && page === 'help' && <HelpPage />}
      </main>
      {message && (
        <div className={`toast ${message.kind}`} role="status">
          <span>{message.text}</span>
          <button className="btn ghost sm" onClick={() => useLab.setState({ message: null })}>×</button>
        </div>
      )}
    </div>
  );
}
