// End-to-end check of the built website in a real browser (Microsoft Edge or Google Chrome, headless).
//   npm run build && npm run e2e
// Starts `vite preview`, drives both modes, verifies exports, and writes screenshots to e2e-output/.
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { chromium } from 'playwright-core';

const OUT = join(process.cwd(), 'e2e-output');
mkdirSync(OUT, { recursive: true });
const browsers = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe', '/usr/bin/google-chrome', '/usr/bin/chromium',
];
const executablePath = process.env.BROWSER ?? browsers.find((p) => existsSync(p));
if (!executablePath) { console.error('No Edge/Chrome found; set BROWSER=path'); process.exit(2); }

const results = [];
// Playwright can reject a pending navigation promise while the browser shuts down; that is not a test result.
const ignoreClosed = (e) => { if (!String(e).includes('has been closed')) { console.error(e); process.exit(1); } };
process.on('unhandledRejection', ignoreClosed);
process.on('uncaughtException', ignoreClosed);
const check = (name, ok, detail = '') => { results.push({ name, ok, detail }); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`); };

const server = spawn(process.execPath, ['node_modules/vite/bin/vite.js', 'preview', '--port', '4317', '--strictPort'], { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error('preview server did not start')), 20000);
  server.stdout.on('data', (d) => { if (String(d).includes('4317')) { clearTimeout(timer); resolve(); } });
});

const browser = await chromium.launch({ executablePath, headless: true });
const context = await browser.newContext({ viewport: { width: 1600, height: 1000 }, acceptDownloads: true });
const page = await context.newPage();
const consoleErrors = [];
page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
page.on('pageerror', (e) => consoleErrors.push(String(e)));

try {
  await page.goto('http://localhost:4317/#/lab');
  await page.getByText(/C engine · webassembly · API v1/).waitFor({ timeout: 20000 });
  check('website loads the C engine (WebAssembly) in a Web Worker', true);

  // ---- Network Simulation: ideal transfer on the demo network --------------------------------------
  await page.getByRole('button', { name: 'Demo network' }).click();
  await page.getByLabel('Simulation speed').fill('2'); // 0.05x real time: slow motion for the screenshot
  await page.getByLabel('ARQ protocol').selectOption('go_back_n');
  await page.getByLabel('Packets').fill('12');
  await page.getByRole('button', { name: '▶ Start' }).click();
  await page.waitForTimeout(2500);
  const inFlight = await page.locator('.react-flow__edge g circle, .react-flow__edge g rect').count();
  check('packets and ACKs are drawn on the topology while running', inFlight > 0, `${inFlight} frames drawn`);
  await page.screenshot({ path: join(OUT, '01-lab-running.png') });
  await page.getByLabel('Simulation speed').fill('8'); // 5x real time
  await page.locator('.panel-head .tag', { hasText: 'Complete' }).waitFor({ timeout: 30000 });
  const strip = await page.locator('.status-strip').innerText();
  check('live transfer completes in the lab', /Delivered\s*12\/12/.test(strip), strip.replace(/\s+/g, ' ').slice(0, 120));

  // ---- Deterministic packet loss via the fault-rule editor -----------------------------------------
  await page.getByLabel('Fault rule type').selectOption('drop_data');
  await page.getByRole('button', { name: '+ Add fault rule' }).click();
  await page.getByLabel('ARQ protocol').selectOption('selective_repeat');
  await page.getByRole('button', { name: /Start|Restart/ }).first().click();
  await page.locator('.status-strip', { hasText: /Selective Repeat\s*State Complete/ }).waitFor({ timeout: 30000 });
  await page.getByRole('tab', { name: /Fault ledger/ }).click();
  const ledger = await page.locator('table.data').last().innerText();
  check('deterministic DATA loss of seq 3 appears in the fault ledger and is recovered', /Data Loss\s+scenario\s+DATA\s+3\s+1/.test(ledger) && /yes @/.test(ledger), ledger.split('\n')[1]?.replace(/\s+/g, ' '));
  await page.getByRole('tab', { name: 'Event log' }).click();
  const log = await page.locator('.log').innerText();
  check('event log shows the loss and the selective retransmission', /DATA seq 3 \(attempt 1\) lost in transit/.test(log) && /retransmits DATA seq 3 \(attempt 2, cause: timeout\)/.test(log));
  await page.getByRole('tab', { name: 'Sender / receiver windows' }).click();
  await page.screenshot({ path: join(OUT, '02-lab-fault-windows.png') });

  // ---- Protocol Comparison mode ---------------------------------------------------------------------
  await page.getByRole('navigation', { name: 'Modes' }).getByText('Protocol Comparison').click();
  await page.getByText('Protocol Comparison Laboratory').waitFor();
  const card = (title) => page.locator('.preset', { hasText: title });
  await card('1. Ideal network').getByRole('button', { name: 'Run' }).click();
  await page.getByText('What the measurements show').waitFor({ timeout: 60000 });
  const findings = await page.locator('.findings').innerText();
  check('ideal-network preset: all runs complete with zero retransmissions', /All 3 runs completed/.test(findings) && /data retransmissions: .*0\.0 = .*0\.0 = .*0\.0/.test(findings));
  await page.screenshot({ path: join(OUT, '03-compare-ideal.png'), fullPage: true });

  await card('4. ACK loss').getByRole('button', { name: 'Run' }).click();
  await page.getByText(/sweep ack_loss_probability|Sweep: Ack Loss Probability/).first().waitFor({ timeout: 60000 });
  await page.getByRole('tab', { name: 'Captured traces' }).click();
  await page.getByLabel('Captured run').selectOption({ index: 0 });
  const ackTrace = await page.locator('.log').innerText();
  check('ACK-loss preset: ACK for seq 3 lost after the data was accepted, duplicate suppressed',
    ackTrace.indexOf('Receiver accepts DATA seq 3') < ackTrace.indexOf('ACK for seq 3 lost') && /Duplicate DATA seq 3 discarded/.test(ackTrace));

  await card('5. Go-Back-N versus Selective Repeat').getByRole('button', { name: 'Run' }).click();
  await page.getByText('Trace evidence', { exact: false }).first().waitFor({ timeout: 60000 });
  await page.getByRole('tab', { name: 'Retransmission evidence' }).click();
  const evidence = await page.locator('table.data').last().innerText();
  check('GBN vs SR: GBN resends 4..11, SR resends only 4', /Go-Back-N[\s\S]*4, 5, 6, 7, 8, 9, 10, 11/.test(evidence) && /Selective Repeat\s+1\s/.test(evidence));
  await page.getByRole('tab', { name: 'Charts' }).click();
  await page.screenshot({ path: join(OUT, '04-compare-gbn-vs-sr.png'), fullPage: true });

  // ---- Full comparison with repeated trials and exports -------------------------------------------
  await card('2. Increasing data loss').getByRole('button', { name: 'Run' }).click();
  await page.getByText(/Aggregate of 10 independent trials/).waitFor({ timeout: 120000 });
  await page.screenshot({ path: join(OUT, '05-compare-loss-sweep.png'), fullPage: true });
  const [csvDownload] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Export CSV' }).click()]);
  const csv = readFileSync(await csvDownload.path(), 'utf8').trim().split('\n');
  const [jsonDownload] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Export JSON' }).click()]);
  const json = JSON.parse(readFileSync(await jsonDownload.path(), 'utf8'));
  check('CSV export has one row per engine run', csv.length === json.runs.length + 1 && json.runs.length === 150, `${csv.length - 1} rows, ${json.runs.length} runs`);
  const header = csv[0].split(',');
  const g = header.indexOf('goodput_bps');
  check('CSV values equal JSON values', json.runs.every((r, i) => Math.abs(Number(csv[i + 1].split(',')[g]) - r.metrics.goodput_bps) < 1e-3 * Math.max(1, r.metrics.goodput_bps)));
  const [report] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Download report' }).click()]);
  const html = readFileSync(await report.path(), 'utf8');
  check('HTML report downloads with findings and per-run table', html.includes('NetRescue protocol comparison report') && html.includes('Per-run results'));

  await page.getByRole('navigation', { name: 'Modes' }).getByText('Experiment History').click();
  await page.locator('table.data tbody tr').first().waitFor();
  check('experiments are saved to history', (await page.locator('table.data tbody tr').count()) >= 4);
  check('no browser console errors', consoleErrors.length === 0, consoleErrors.slice(0, 3).join(' | '));
} catch (error) {
  check('e2e run', false, String(error));
  await page.screenshot({ path: join(OUT, 'failure.png'), fullPage: true });
} finally {
  await context.close().catch(() => undefined);
  await browser.close().catch(() => undefined);
  server.kill();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`\n${results.length - failed}/${results.length} end-to-end checks passed. Screenshots: ${OUT}`);
process.exit(failed ? 1 : 0);
