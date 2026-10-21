/* W-b live bridge check (M4b real-machine window; one-off, not a committed gate).
 * Drives the REAL bridge.mjs against the LIVE creo-hub inside Creo. */
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
import path from 'node:path';

const ROOT = process.env.CREO_HUB_ROOT || process.cwd();
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('[PASS] ' + m); } else { fail++; console.log('[FAIL] ' + m); } };

const transport = new StdioClientTransport({
  command: 'node',
  args: [path.join(ROOT, 'bridge', 'bridge.mjs')],
  env: { ...process.env, CREO_HUB_ROOT: ROOT },
});
const client = new Client({ name: 'wb-live', version: '1.0.0' }, { capabilities: {} });
await client.connect(transport);

const call = async (name, args) => {
  const r = await client.callTool({ name, arguments: args || {} });
  const text = r.content && r.content[0] && r.content[0].text;
  let json = null;
  try { json = JSON.parse(text); } catch { json = { _raw: text }; }
  return { isError: r.isError === true, json, text };
};

const tools = (await client.listTools()).tools;
ok(tools.some(t => t.name === 'creo_hub_job'), 'tools list includes creo_hub_job');
ok(tools.some(t => t.name === 'creo_hub_status'), 'tools list includes creo_hub_status');

let r = await call('creo_hub_status');
ok(r.json && r.json.ok === true && r.json.hubVersion === '1.0.0', 'status hubVersion=1.0.0 (got ' + (r.json && r.json.hubVersion) + ')');
ok(r.json && r.json.jobs && typeof r.json.jobs.running === 'number', 'status carries jobs counters');
ok(r.json && Array.isArray(r.json.features) && r.json.features.includes('jobs'), 'status features include jobs');

r = await call('creo_hub_call', { plugin: 'demo3', command: 'demo3.longtask', args: { steps: 4, ms: 250 } });
ok(r.json && r.json.ok === true && r.json.async === true && /^j-\d+$/.test(String(r.json.jobId)), 'live async call -> async jobId (' + (r.json && r.json.jobId) + ')');
const jobId = r.json.jobId;

let st = null, jr = null, samples = [];
for (let i = 0; i < 80; i++) {
  r = await call('creo_hub_job', { jobId });
  const j = r.json && r.json.job;
  if (j && j.state !== st) { st = j.state; samples.push(st + '@' + j.elapsedMs + 'ms pct=' + j.pct); }
  if (j && (j.state === 'completed' || j.state === 'failed')) { jr = j; break; }
  await new Promise(res => setTimeout(res, 150));
}
ok(jr && jr.state === 'completed', 'job completed (samples: ' + samples.join(' -> ') + ')');
ok(jr && jr.result && jr.result.ok === true && JSON.stringify(jr.result).indexOf('"steps":4') >= 0, 'job carries plugin result (steps=4, nested envelope)');
ok(jr && jr.pct === 100, 'job pct=100 at completion');

r = await call('creo_hub_job', {});
ok(r.json && r.json.ok === true && Array.isArray(r.json.jobs) && r.json.jobs.some(j => j.jobId === jobId), 'job list contains the job');

r = await call('creo_hub_job', { jobId: 'j-999999' });
ok(r.json && r.json.ok === false && /job not found/i.test(r.json.error || ''), 'unknown job -> structured not-found');

await client.close();
console.log(`\n=== W-B LIVE BRIDGE: ${pass} pass, ${fail} fail ===`);
process.exit(fail ? 1 : 0);
