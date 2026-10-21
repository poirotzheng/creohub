#!/usr/bin/env node
/**
 * agent/hub-agent.mjs — creo-hub 形态 C 最小闭环（M4d，docs\39 §7）
 *
 * 零 npm 依赖（Node >=18 内置模块）+ 复用 bridge/lib/hub_client.mjs（端口发现/
 * 令牌/错误映射 = 单一真源）。职责：
 *   - 本地 HTTP（127.0.0.1:8776..8785）+ 令牌校验（hub_token.txt）+ CORS 白名单
 *   - OpenAI 兼容 /chat/completions + tools 多轮循环（模型端点可配：公网/内网）
 *   - 工具面固定 5 个（status/plugins/manifest/call/job），descForLLM 直通
 *   - 428 批准链：nonce **不进模型上下文**（agent 内存持有，批准后同参重试）
 *   - async 命令：202 -> 轮询 job 至终态再汇报
 *
 * 硬约束（继承）：hub DLL 永不外连 LLM；API key 只在 agent.config.json；
 * 日志不落令牌/key/完整 args；只绑环回。ES5 面板直连本进程（CORS 白名单）。
 *
 * 用法：node agent\hub-agent.mjs [--port 8776] [--config agent\agent.config.json]
 * 端点：GET /api/agent/health  POST /api/chat
 */
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, appendFileSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { BridgeError, discover, get, getStatus, postCall, resolveRoot } from '../bridge/lib/hub_client.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const LOG = join(HERE, 'agent.log');
const MAX_BODY = 256 * 1024;
const CHAT_TIMEOUT_MS = 120000;
const TOOL_TIMEOUT_MS = 120000;

/* ---------------------------------------------------------------- config */
const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const CFG_PATH = resolve(opt('--config', join(HERE, 'agent.config.json')));
const PORT_START = Number(opt('--port', 0)) || 8776;

function loadConfig() {
  let c = {};
  try { c = JSON.parse(readFileSync(CFG_PATH, 'utf8')); } catch { c = {}; }
  return {
    baseURL: String(c.baseURL || process.env.CREO_HUB_LLM_BASEURL || ''),
    apiKey: String(c.apiKey || process.env.CREO_HUB_LLM_KEY || ''),
    model: String(c.model || 'deepseek-chat'),
    temperature: typeof c.temperature === 'number' ? c.temperature : 0.2,
    maxRounds: Number.isInteger(c.maxRounds) ? c.maxRounds : 6,
    maxTokens: Number.isInteger(c.maxTokens) ? c.maxTokens : 1024,
    jobPollMs: Number.isInteger(c.jobPollMs) ? c.jobPollMs : 60000,
    toolMode: c.toolMode === 'json' ? 'json' : 'native',
  };
}
let cfg = loadConfig();

/* ------------------------------------------------------------------ log */
function log(obj) {
  try { appendFileSync(LOG, JSON.stringify({ t: new Date().toISOString(), ...obj }) + '\n'); } catch { /* best effort */ }
}

/* -------------------------------------------------------------- hub token */
function hubToken() { return readFileSync(join(resolveRoot(), 'hub_token.txt'), 'utf8').trim(); }

/* ---------------------------------------------------------------- tools */
const TOOLS = [
  { type: 'function', function: { name: 'creo_hub_status', description: 'Hub connectivity/version/features/job counters. Call first when anything fails.', parameters: { type: 'object', properties: {}, additionalProperties: false } } },
  { type: 'function', function: { name: 'creo_hub_plugins', description: 'Plugin state cards (state/requires/provides/invalidReason). Ground ids before mount/unmount.', parameters: { type: 'object', properties: {}, additionalProperties: false } } },
  { type: 'function', function: { name: 'creo_hub_manifest', description: 'Single source of truth for command contracts (risk/inputSchema/descForLLM). Read before calling.', parameters: { type: 'object', properties: {}, additionalProperties: false } } },
  { type: 'function', function: { name: 'creo_hub_call', description: 'Execute one plugin command through the hub gate. confirm/destructive commands may return needConfirm: a human must approve in the Creo panel; then RETRY the same call with identical args.', parameters: { type: 'object', properties: { plugin: { type: 'string' }, command: { type: 'string' }, args: { type: 'object' } }, required: ['plugin', 'command'], additionalProperties: false } } },
  { type: 'function', function: { name: 'creo_hub_job', description: 'Poll an async job (a call may answer 202 + jobId instead of a result). Omit jobId to list jobs.', parameters: { type: 'object', properties: { jobId: { type: 'string' } }, additionalProperties: false } } },
];

const SYSTEM = [
  'You operate a Creo 6 session through the creo-hub gate. Hard rules:',
  '1) Query before change: creo_hub_manifest/plugins first; use exact ids (never invent commands).',
  '2) Never retry a mutating command blindly. If a call times out or returns a jobId, inspect job/status first.',
  '3) confirm/destructive commands are human-approved in the Creo panel. When you get needConfirm, ask the user to approve, then retry the SAME command with identical args. Never ask for or reveal nonces.',
  '4) Report honestly: tool failures are data; quote err_code/hint. Do not claim success without a tool result.',
  '5) Geometry is created only by explicit commands, never by you. Answer in the user language.',
].join('\n');

/* --------------------------------------------------------- pending approvals */
/* {key, nonce, txn, ts} - nonce stays INSIDE the agent (never in messages). */
const pending = new Map();
const digestKey = (plugin, command, args) => plugin + '|' + command + '|' + JSON.stringify(args || {});

async function hubPendingApproved(plugin, command) {
  try {
    const r = await get('/api/v1/pending');
    const tickets = Array.isArray(r.tickets) ? r.tickets : (Array.isArray(r.pending) ? r.pending : []);
    return tickets.some((t) => t && t.plugin === plugin && t.command === command &&
      (t.state === 'approved' || t.state === 'APPROVED'));
  } catch { return false; }
}

/* --------------------------------------------------------------- exec tool */
async function runTool(name, a) {
  const started = Date.now();
  try {
    let out;
    if (name === 'creo_hub_status') {
      const { port, health } = await discover({ pin: false });
      out = { ok: true, port, apiVersion: health.apiVersion, hubVersion: health.hubVersion, features: health.features, executorReady: health.executorReady, orphaned: health.orphaned === true, jobs: health.jobs };
    } else if (name === 'creo_hub_plugins') out = await get('/api/v1/plugins');
    else if (name === 'creo_hub_manifest') out = await get('/api/v1/manifest');
    else if (name === 'creo_hub_job') {
      const id = a && typeof a.jobId === 'string' ? a.jobId.trim() : '';
      const r = await getStatus(id ? `/api/v1/jobs/${id}` : '/api/v1/jobs?all=1');
      out = r.json;
    } else if (name === 'creo_hub_call') {
      if (!a || typeof a.plugin !== 'string' || typeof a.command !== 'string') {
        return { ok: false, error: 'BAD_ARGS', hint: 'plugin and command are required' };
      }
      const key = digestKey(a.plugin, a.command, a.args);
      const p = pending.get(key);
      const opts = {};
      if (p) {
        const ok = await hubPendingApproved(a.plugin, a.command);
        if (ok) opts.confirmNonce = p.nonce;
      }
      const { status, json } = await postCall(a.plugin, a.command, a.args, opts);
      if (status === 428) {
        const nc = json && json.needConfirm;
        if (nc && typeof nc.nonce === 'string') pending.set(key, { nonce: nc.nonce, txn: nc.transactionId, ts: Date.now() });
        out = {
          ok: false, awaitingConfirm: true, risk: nc && nc.risk, command: nc && nc.command,
          argsDigest: nc && nc.argsDigest, expiresMs: nc && nc.expiresMs,
          hint: 'human approval required: approve this command in the Creo hub panel, then retry the SAME command with identical args',
        };
      } else if (status === 202) {
        out = { ok: true, async: true, jobId: json && json.result && json.result.jobId, poll: json && json.poll, meta: json && json.meta,
          hint: 'async accepted - poll creo_hub_job and report the final result' };
      } else if (status === 200) {
        if (p) pending.delete(key);
        out = json;
      } else {
        out = { ...(json || {}), httpStatus: status };
      }
    } else {
      out = { ok: false, error: 'UNKNOWN_TOOL', hint: 'tool not supported' };
    }
    const ms = Date.now() - started;
    log({ ev: 'tool', name, ms, ok: !!(out && out.ok), err: out && out.error });
    return out;
  } catch (e) {
    const err = e instanceof BridgeError ? e.toJSON() : { ok: false, err_code: 'AGENT_TOOL_FAIL', hint: String((e && e.message) || e).slice(0, 200) };
    log({ ev: 'tool', name, ok: false, err: err.err_code });
    return err;
  }
}

/* ---------------------------------------------------------------- LLM loop */
/* toolMode:
 *  - "native" (default): OpenAI function calling (vLLM/SGLang/DeepSeek/LM Studio/Ollama).
 *  - "json": fallback for servers/models WITHOUT tools support - the model is
 *    asked to answer one JSON instruction per turn; the agent parses it.
 * Both modes drive the identical runTool() gate. */
const JSON_PROTOCOL = [
  '',
  'TOOL PROTOCOL (this server does not use native tool calls):',
  'Answer with ONE JSON object per turn, no prose, no code fences:',
  '  {"tool":"<name>","args":{...}}   to call a tool (the tool result comes back as a user message)',
  '  {"final":"<answer>"}             when done',
  'Available tools: creo_hub_status, creo_hub_plugins, creo_hub_manifest, creo_hub_call{plugin,command,args?}, creo_hub_job{jobId?}.',
].join('\n');

async function chatOnce(messages) {
  if (!cfg.baseURL) {
    return { ok: false, err_code: 'LLM_NOT_CONFIGURED', hint: 'set baseURL/apiKey/model in agent\\agent.config.json (OpenAI-compatible endpoint)' };
  }
  const url = cfg.baseURL.replace(/\/+$/, '') + '/chat/completions';
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), CHAT_TIMEOUT_MS);
  const payload = { model: cfg.model, messages, temperature: cfg.temperature, max_tokens: cfg.maxTokens };
  if (cfg.toolMode === 'native') { payload.tools = TOOLS; payload.tool_choice = 'auto'; }
  try {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...(cfg.apiKey ? { Authorization: 'Bearer ' + cfg.apiKey } : {}) },
      body: JSON.stringify(payload),
      signal: ctl.signal,
    });
    const text = await res.text();
    if (!res.ok) {
      return { ok: false, err_code: 'LLM_HTTP_' + res.status, hint: text.slice(0, 300) };
    }
    let j = null;
    try { j = JSON.parse(text); } catch { return { ok: false, err_code: 'LLM_BAD_JSON', hint: text.slice(0, 200) }; }
    const msg = j && j.choices && j.choices[0] && j.choices[0].message;
    if (!msg) return { ok: false, err_code: 'LLM_EMPTY', hint: 'no choices[0].message in response' };
    return { ok: true, message: msg, usage: j.usage };
  } catch (e) {
    return { ok: false, err_code: e.name === 'AbortError' ? 'LLM_TIMEOUT' : 'LLM_TRANSPORT', hint: String((e && e.message) || e).slice(0, 200) };
  } finally {
    clearTimeout(timer);
  }
}

async function runAgent(userMessages) {
  const sys = cfg.toolMode === 'json' ? SYSTEM + JSON_PROTOCOL : SYSTEM;
  const messages = [{ role: 'system', content: sys }, ...userMessages];
  const trace = [];
  let lastConfirm = null;
  for (let round = 0; round < cfg.maxRounds; round++) {
    const r = await chatOnce(messages);
    if (!r.ok) return { ok: false, err_code: r.err_code, hint: r.hint, toolTrace: trace };
    const msg = r.message;

    /* ---- json fallback: parse ONE instruction object from content ---- */
    if (cfg.toolMode === 'json') {
      let instr = null;
      try {
        const m = String(msg.content || '').match(/\{[\s\S]*\}/);
        if (m) instr = JSON.parse(m[0]);
      } catch { instr = null; }
      messages.push({ role: 'assistant', content: String(msg.content || '') });
      if (!instr) {
        return { ok: true, reply: String(msg.content || ''), toolTrace: trace, usage: r.usage, rounds: round + 1, lastConfirm };
      }
      if (typeof instr.final === 'string') {
        return { ok: true, reply: instr.final, toolTrace: trace, usage: r.usage, rounds: round + 1, lastConfirm };
      }
      if (typeof instr.tool === 'string') {
        const out = await runTool(instr.tool, instr.args && typeof instr.args === 'object' ? instr.args : {});
        if (out && out.awaitingConfirm) lastConfirm = out;
        trace.push({ tool: instr.tool, ok: !!(out && out.ok), err: out && (out.err_code || out.error || (out.awaitingConfirm ? 'awaitingConfirm' : undefined)), ms: Date.now() });
        messages.push({ role: 'user', content: 'TOOL_RESULT ' + JSON.stringify(out).slice(0, 20000) + '\nAnswer with the next JSON instruction.' });
        continue;
      }
      return { ok: true, reply: String(msg.content || ''), toolTrace: trace, usage: r.usage, rounds: round + 1, lastConfirm };
    }

    /* ---- native function calling ---- */
    messages.push(msg);
    const calls = Array.isArray(msg.tool_calls) ? msg.tool_calls : [];
    if (calls.length === 0) {
      return { ok: true, reply: String(msg.content || ''), toolTrace: trace, usage: r.usage, rounds: round + 1, lastConfirm };
    }
    for (const c of calls) {
      const name = c.function && c.function.name;
      let args = {};
      try { args = JSON.parse((c.function && c.function.arguments) || '{}'); } catch { args = {}; }
      const out = await runTool(name, args);
      if (out && out.awaitingConfirm) lastConfirm = out;
      trace.push({ tool: name, ok: !!(out && out.ok), err: out && (out.err_code || out.error || (out.awaitingConfirm ? 'awaitingConfirm' : undefined)), ms: Date.now() });
      messages.push({ role: 'tool', tool_call_id: c.id, content: JSON.stringify(out).slice(0, 20000) });
    }
  }
  return { ok: false, err_code: 'AGENT_MAX_ROUNDS', hint: 'tool loop exceeded maxRounds (' + cfg.maxRounds + ')', toolTrace: trace };
}

/* ------------------------------------------------------------------- HTTP */
function cors(origin) {
  if (!origin) return null;
  try {
    const u = new URL(origin);
    if ((u.hostname === '127.0.0.1' || u.hostname === 'localhost' || u.hostname === '[::1]') && u.protocol === 'http:') return origin;
  } catch { /* ignore */ }
  return null;
}
function send(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}
function readBody(req) {
  return new Promise((resolveBody) => {
    let n = 0; const chunks = [];
    req.on('data', (c) => { n += c.length; if (n > MAX_BODY) { resolveBody(null); req.destroy(); } else chunks.push(c); });
    req.on('end', () => resolveBody(Buffer.concat(chunks).toString('utf8')));
    req.on('error', () => resolveBody(null));
  });
}

const server = createServer(async (req, res) => {
  const origin = req.headers.origin || '';
  const allow = cors(origin);
  if (origin && !allow) { send(res, 403, { ok: false, error: 'origin not allowed' }); return; }
  if (allow) {
    res.setHeader('Access-Control-Allow-Origin', allow);
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-Hub-Token');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  }
  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }
  const url = new URL(req.url, 'http://127.0.0.1');
  if (url.pathname === '/api/agent/health' && req.method === 'GET') {
    let hub = null;
    try { const d = await discover({ pin: false }); hub = { port: d.port, features: d.health.features, hubVersion: d.health.hubVersion }; } catch { hub = null; }
    send(res, 200, { ok: true, hub, model: cfg.baseURL ? { baseURL: cfg.baseURL, model: cfg.model, toolMode: cfg.toolMode } : null, rounds: cfg.maxRounds, pending: pending.size });
    return;
  }
  /* ---- config face (M4d+): panel "模型设置" writes the SAME config file ---- */
  if (url.pathname === '/api/agent/config' && (req.method === 'GET' || req.method === 'POST')) {
    let tok0 = '';
    try { tok0 = hubToken(); } catch { send(res, 500, { ok: false, err_code: 'TOKEN_MISSING', hint: 'hub_token.txt not found' }); return; }
    if (!req.headers['x-hub-token'] || String(req.headers['x-hub-token']).trim() !== tok0) { send(res, 401, { ok: false, error: 'unauthorized' }); return; }
    if (req.method === 'GET') {
      send(res, 200, { ok: true, config: { baseURL: cfg.baseURL, model: cfg.model, toolMode: cfg.toolMode, maxRounds: cfg.maxRounds, hasKey: !!cfg.apiKey } });
      return;
    }
    const rawC = await readBody(req);
    let b = null;
    try { b = JSON.parse(rawC || '{}'); } catch { send(res, 400, { ok: false, error: 'invalid json body' }); return; }
    const next = { ...cfg };
    if (typeof b.baseURL === 'string') next.baseURL = b.baseURL.trim();
    if (typeof b.model === 'string' && b.model.trim()) next.model = b.model.trim();
    if (typeof b.apiKey === 'string') next.apiKey = b.apiKey;
    if (typeof b.toolMode === 'string') next.toolMode = b.toolMode === 'json' ? 'json' : 'native';
    if (Number.isInteger(b.maxRounds)) next.maxRounds = Math.max(1, Math.min(12, b.maxRounds));
    /* validation: the #1 user mistake is pointing at the hub panel itself */
    let u = null;
    try { u = new URL(next.baseURL); } catch { u = null; }
    if (!u || (u.protocol !== 'http:' && u.protocol !== 'https:')) {
      send(res, 400, { ok: false, err_code: 'BAD_BASEURL', hint: 'baseURL must be http(s)://host[:port][/v1] - e.g. http://10.0.0.5:8000/v1' });
      return;
    }
    if ((u.hostname === '127.0.0.1' || u.hostname === 'localhost') && Number(u.port) >= 8765 && Number(u.port) <= 8775) {
      send(res, 400, { ok: false, err_code: 'HUB_PORT_CONFLICT', hint: 'that port is the creo-hub PANEL (not a model). Use your model server URL, e.g. http://<model-host>:8000/v1' });
      return;
    }
    try {
      writeFileSync(CFG_PATH, JSON.stringify({
        baseURL: next.baseURL, apiKey: next.apiKey, model: next.model,
        temperature: next.temperature, maxRounds: next.maxRounds, maxTokens: next.maxTokens,
        jobPollMs: next.jobPollMs, toolMode: next.toolMode,
      }, null, 2) + '\n');
      cfg = loadConfig();
    } catch (e) {
      send(res, 500, { ok: false, err_code: 'CONFIG_WRITE_FAIL', hint: String((e && e.message) || e).slice(0, 160) });
      return;
    }
    let test = null;
    if (b.test === true) {
      const t = await chatOnce([{ role: 'user', content: 'ping' }]);
      test = { ok: t.ok, err_code: t.err_code, hint: t.hint };
    }
    log({ ev: 'config', ok: true, baseURL: cfg.baseURL, model: cfg.model, toolMode: cfg.toolMode, test: test ? test.ok : null });
    send(res, 200, { ok: true, saved: true, config: { baseURL: cfg.baseURL, model: cfg.model, toolMode: cfg.toolMode, maxRounds: cfg.maxRounds, hasKey: !!cfg.apiKey }, test });
    return;
  }

  if (url.pathname !== '/api/chat' || req.method !== 'POST') { send(res, 404, { ok: false, error: 'not found' }); return; }

  /* token gate (same trust domain as the hub: hub_token.txt) */
  let tok = '';
  try { tok = hubToken(); } catch { send(res, 500, { ok: false, err_code: 'TOKEN_MISSING', hint: 'hub_token.txt not found (start the hub once)' }); return; }
  if (!req.headers['x-hub-token'] || String(req.headers['x-hub-token']).trim() !== tok) { send(res, 401, { ok: false, error: 'unauthorized' }); return; }

  const raw = await readBody(req);
  if (!raw) { send(res, 413, { ok: false, error: 'body too large' }); return; }
  let body = null;
  try { body = JSON.parse(raw); } catch { send(res, 400, { ok: false, error: 'invalid json body' }); return; }
  const msgs = body && Array.isArray(body.messages) ? body.messages : null;
  if (!msgs || msgs.length === 0) { send(res, 400, { ok: false, error: 'messages[] is required' }); return; }
  const clean = msgs.filter((m) => m && (m.role === 'user' || m.role === 'assistant') && typeof m.content === 'string')
    .slice(-20).map((m) => ({ role: m.role, content: String(m.content).slice(0, 8000) }));

  try {
    const out = await runAgent(clean);
    log({ ev: 'chat', ok: out.ok, rounds: out.rounds, tools: (out.toolTrace || []).map((t) => t.tool) });
    send(res, out.ok ? 200 : 502, out);
  } catch (e) {
    log({ ev: 'chat', ok: false, err: String((e && e.message) || e).slice(0, 160) });
    send(res, 500, { ok: false, err_code: 'AGENT_INTERNAL', hint: 'agent internal error (see agent/agent.log)' });
  }
});

/* listen on the first free port in the range (loopback only) */
let port = 0;
for (let p = PORT_START; p < PORT_START + 10; p++) {
  const ok = await new Promise((r) => {
    server.once('error', () => r(false));
    server.listen(p, '127.0.0.1', () => r(true));
  });
  if (ok) { port = p; break; }
  await new Promise((r) => setTimeout(r, 20));
}
if (!port) { console.error('[hub-agent] no free port in range ' + PORT_START + '..' + (PORT_START + 9)); process.exit(1); }
console.log('[hub-agent] listening on http://127.0.0.1:' + port + ' (config: ' + CFG_PATH + ')');
if (!cfg.baseURL) console.log('[hub-agent] WARNING: no LLM baseURL configured - /api/chat will answer LLM_NOT_CONFIGURED');
