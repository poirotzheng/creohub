/* agent offline test (M4d) - mock hub + fake OpenAI server + real agent process.
 * Verifies: auth (token), CORS allowlist, tool loop, structured failure surface.
 * No Creo, no real LLM, no external deps.  Exit 0 = ALL PASS. */
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

const ROOT = new URL('../../', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const AGENT = join(ROOT, 'agent', 'hub-agent.mjs');
const WORK = join(tmpdir(), 'creo_hub_agent_test');
const TOKEN = 'a1b2c3d4e5f60718293a4b5c6d7e8f90';
const HUB_PORT = 8774;
const LLM_PORT = 8790;
const AGENT_PORT = 8777;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('[PASS] ' + m); } else { fail++; console.log('[FAIL] ' + m); } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

rmSync(WORK, { recursive: true, force: true });
mkdirSync(WORK, { recursive: true });
writeFileSync(join(WORK, 'hub_token.txt'), TOKEN + '\n');

/* ---------------------------------------------------------- mock hub */
const hub = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const send = (o) => { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(o)); };
  if (url.pathname === '/api/v1/health') return send({ ok: true, apiVersion: 'v1', hubVersion: '1.0.0', features: ['panel', 'manifest', 'call', 'versioning', 'jobs'], port: HUB_PORT, uptimeSec: 1, executorReady: true, queueUsed: 0, jobs: { running: 0, queued: 0 } });
  if (url.pathname === '/api/v1/manifest') return send({ ok: true, apiVersion: 'v1', plugins: [{ id: 'p-ok', manifestVersion: 2, commands: [{ id: 'p-ok.hello', risk: 'safe', descForLLM: 'hello fixture', inputSchema: { type: 'object', properties: {} } }] }] });
  if (url.pathname === '/api/v1/plugins') return send({ ok: true, plugins: [{ id: 'p-ok', state: 'ACTIVE' }], count: 1 });
  if (url.pathname === '/api/v1/pending') return send({ ok: true, tickets: [] });
  if (url.pathname.startsWith('/api/v1/jobs')) return send({ ok: true, job: { jobId: 'j-1', state: 'completed', pct: 100, result: { ok: true } } });
  if (url.pathname === '/api/v1/call' && req.method === 'POST') return send({ ok: true, result: { echo: 'hello', n: 1 }, ms: 3 });
  send({ ok: false, error: 'not found' });
});
await new Promise((r) => hub.listen(HUB_PORT, '127.0.0.1', r));
console.log('[mock hub] ' + HUB_PORT);

/* ---------------------------------------------------------- fake LLM */
let llmRound = 0;
let llmJsonRound = 0;
const llm = createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    const native = body.indexOf('"tools"') >= 0;
    if (!native) {
      llmJsonRound++;
      const content = llmJsonRound === 1
        ? '{"tool":"creo_hub_manifest","args":{}}'
        : '{"final":"json 模式完成（p-ok）"}';
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ id: 'j' + llmJsonRound, object: 'chat.completion', model: 'fake', choices: [{ index: 0, message: { role: 'assistant', content }, finish_reason: 'stop' }], usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 } }));
      return;
    }
    llmRound++;
    const base = { id: 'r' + llmRound, object: 'chat.completion', model: 'fake' };
    if (llmRound === 1) {
      base.choices = [{ index: 0, message: { role: 'assistant', content: '', tool_calls: [{ id: 'call_1', type: 'function', function: { name: 'creo_hub_manifest', arguments: '{}' } }] }, finish_reason: 'tool_calls' }];
    } else {
      base.choices = [{ index: 0, message: { role: 'assistant', content: '插件 p-ok 状态正常（fixture）' }, finish_reason: 'stop' }];
    }
    base.usage = { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 };
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(base));
  });
});
await new Promise((r) => llm.listen(LLM_PORT, '127.0.0.1', r));
console.log('[mock llm] ' + LLM_PORT);

/* ---------------------------------------------------------- agent */
writeFileSync(join(WORK, 'agent.config.json'), JSON.stringify({ baseURL: 'http://127.0.0.1:' + LLM_PORT + '/v1', apiKey: 'k', model: 'fake', maxRounds: 3 }));
const agent = spawn(process.execPath, [AGENT, '--port', String(AGENT_PORT), '--config', join(WORK, 'agent.config.json')], {
  env: { ...process.env, CREO_HUB_ROOT: WORK },
  stdio: ['ignore', 'pipe', 'pipe'],
});
let agentOut = '';
agent.stdout.on('data', (d) => { agentOut += d.toString(); });
agent.stderr.on('data', (d) => { agentOut += d.toString(); });

async function req(method, path, body, headers) {
  const res = await fetch('http://127.0.0.1:' + AGENT_PORT + path, {
    method, headers: { 'Content-Type': 'application/json', ...(headers || {}) }, body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let json = null; try { json = JSON.parse(text); } catch { /* keep text */ }
  return { status: res.status, json, headers: res.headers, text };
}

let ready = false;
for (let i = 0; i < 60; i++) {
  try { const h = await req('GET', '/api/agent/health'); if (h.status === 200) { ready = true; break; } } catch { /* wait */ }
  await sleep(150);
}
ok(ready, 'agent health ready (port ' + AGENT_PORT + ')');
if (!ready) { agent.kill(); hub.close(); llm.close(); console.log(agentOut); process.exit(1); }

let r = await req('GET', '/api/agent/health');
ok(r.json && r.json.ok === true && r.json.model && r.json.model.model === 'fake', 'health reports model config');

/* auth */
r = await req('POST', '/api/chat', { messages: [{ role: 'user', content: 'hi' }] });
ok(r.status === 401 && r.json && r.json.error === 'unauthorized', 'missing token -> 401');
r = await req('POST', '/api/chat', { messages: [{ role: 'user', content: 'hi' }] }, { 'X-Hub-Token': 'deadbeef' });
ok(r.status === 401, 'wrong token -> 401');

/* tool loop */
r = await req('POST', '/api/chat', { messages: [{ role: 'user', content: '列出插件状态' }] }, { 'X-Hub-Token': TOKEN });
ok(r.status === 200 && r.json && r.json.ok === true, 'chat ok with token');
ok(r.json && typeof r.json.reply === 'string' && r.json.reply.indexOf('p-ok') >= 0, 'reply carries model text (' + (r.json && r.json.reply) + ')');
ok(r.json && Array.isArray(r.json.toolTrace) && r.json.toolTrace.some((t) => t.tool === 'creo_hub_manifest' && t.ok), 'tool loop executed creo_hub_manifest');

/* CORS allowlist */
r = await req('POST', '/api/chat', { messages: [{ role: 'user', content: 'x' }] }, { 'X-Hub-Token': TOKEN, Origin: 'http://127.0.0.1:' + HUB_PORT });
ok(r.status === 200 && String(r.headers.get('access-control-allow-origin') || '') === 'http://127.0.0.1:' + HUB_PORT, 'panel origin allowed (ACAO echo)');
r = await req('POST', '/api/chat', { messages: [{ role: 'user', content: 'x' }] }, { 'X-Hub-Token': TOKEN, Origin: 'http://evil.example' });
ok(r.status === 403, 'foreign origin -> 403');

/* bad body */
r = await req('POST', '/api/chat', { nope: 1 }, { 'X-Hub-Token': TOKEN });
ok(r.status === 400 && /messages/.test(String(r.json && r.json.error)), 'missing messages -> 400');

/* ---- config face: GET mask / reject hub-port / save + test ---- */
r = await req('GET', '/api/agent/config', null, { 'X-Hub-Token': TOKEN });
ok(r.status === 200 && r.json && r.json.config && r.json.config.hasKey === true && r.json.config.baseURL.indexOf('8790') > 0, 'config GET returns masked config');
r = await req('POST', '/api/agent/config', { baseURL: 'http://127.0.0.1:8765/' }, { 'X-Hub-Token': TOKEN });
ok(r.status === 400 && r.json && r.json.err_code === 'HUB_PORT_CONFLICT', 'config POST rejects the hub panel port (the 405 trap)');
r = await req('POST', '/api/agent/config', { baseURL: 'http://127.0.0.1:' + LLM_PORT + '/v1', model: 'fake2', toolMode: 'native', maxRounds: 4, test: true }, { 'X-Hub-Token': TOKEN });
ok(r.status === 200 && r.json && r.json.saved === true && r.json.config.model === 'fake2', 'config POST saved');
ok(r.json && r.json.test && r.json.test.ok === true, 'config POST test=true reached the model');
r = await req('GET', '/api/agent/health');
ok(r.json && r.json.model && r.json.model.toolMode === 'native' && r.json.rounds === 4, 'agent reloaded the saved config');

/* ---- toolMode:"json" fallback (servers without function calling) ---- */
const JSON_PORT = 8778;
writeFileSync(join(WORK, 'agent.json.config.json'), JSON.stringify({ baseURL: 'http://127.0.0.1:' + LLM_PORT + '/v1', apiKey: '', model: 'fake', toolMode: 'json', maxRounds: 4 }));
const agent2 = spawn(process.execPath, [AGENT, '--port', String(JSON_PORT), '--config', join(WORK, 'agent.json.config.json')], {
  env: { ...process.env, CREO_HUB_ROOT: WORK }, stdio: ['ignore', 'pipe', 'pipe'],
});
async function req2(method, path, body, headers) {
  const res = await fetch('http://127.0.0.1:' + JSON_PORT + path, { method, headers: { 'Content-Type': 'application/json', ...(headers || {}) }, body: body ? JSON.stringify(body) : undefined });
  const text = await res.text();
  let json = null; try { json = JSON.parse(text); } catch { /* ignore */ }
  return { status: res.status, json };
}
let ready2 = false;
for (let i = 0; i < 60; i++) {
  try { const h = await req2('GET', '/api/agent/health'); if (h.status === 200) { ready2 = true; break; } } catch { /* wait */ }
  await sleep(150);
}
ok(ready2, 'json-mode agent ready');
r = await req2('POST', '/api/chat', { messages: [{ role: 'user', content: '列出插件' }] }, { 'X-Hub-Token': TOKEN });
ok(r.status === 200 && r.json && r.json.ok === true, 'json-mode chat ok');
ok(r.json && /json 模式完成/.test(String(r.json.reply || '')), 'json-mode final answer parsed (' + (r.json && r.json.reply) + ')');
ok(r.json && Array.isArray(r.json.toolTrace) && r.json.toolTrace.some((t) => t.tool === 'creo_hub_manifest' && t.ok), 'json-mode executed the instructed tool');
agent2.kill();

agent.kill();
hub.close(); llm.close();
await sleep(200);
console.log(`\n=== AGENT TEST: ${pass} pass, ${fail} fail ===`);
process.exit(fail ? 1 : 0);
