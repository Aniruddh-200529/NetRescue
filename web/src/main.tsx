import React from 'react';
import ReactDOM from 'react-dom/client';
import '@xyflow/react/dist/style.css';
import './styles.css';
import { App } from './App';
import { EngineClient, workerTransport } from './engine/client';

const engine = new EngineClient(workerTransport());

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App engine={engine} />
  </React.StrictMode>,
);
