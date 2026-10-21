#!/usr/bin/env node
/* =====================================================================
 * creo-hub M3a E11 通道实验探针（docs\23 §9.1/§9.2 用例表驱动）
 * 用法:
 *   node test\e11_probe.mjs --case <id> [--route A|B] [--enc char|wchar]
 *   id: e11-1..e11-9 | e11-10/mem100 | missing-func(=e11-5) | boom(=e11-9)
 *       | all-b | all-a | list
 * 前置: Creo+hub(dev DLL) 在跑、demo3 已挂载（e11-6 反而要求一个非 ACTIVE 包）。
 * 纪律: 令牌只进内存不进日志；每案一行 JSONL 落 logs\e11_round.log；
 *       退出码 0=全 PASS（REVIEW 不算失败），1=有 FAIL，2=环境不可用。
 * ===================================================================== */
import { readFileSync, appendFileSync, mkdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const LOGF = resolve(ROOT, 'logs', 'e11_round.log');

let TOKEN = '';
try { TOKEN = readFileSync(resolve(ROOT, 'hub_token.txt'), 'utf8').trim(); }
catch {
  /* 令牌文件由 hub 运行时生成、terminate 时删除——离线（list/语法检查）属正常。
   * 真要探活时 findPort 会因无令牌 401 而失败，届时给出清单提示。 */
  console.warn('[WARN] hub_token.txt not readable (hub not running?) - live cases will fail auth');
}

/* ---------------- CLI ---------------- */
const argv = process.argv.slice(2);
function opt(name, dflt) {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
}
const CASE = opt('case', 'list');
const ROUTE = opt('route', null);        /* null = case default / dual */
const ENC = opt('enc', 'char');

/* ---------------- transport ---------------- */
async function findPort() {
  for (let p = 8765; p <= 8775; p++) {
    try {
      const r = await fetch(`http://127.0.0.1:${p}/api/v1/health`,
        { headers: { 'X-Hub-Token': TOKEN } });
      if (r.ok) { const j = await r.json(); if (j) return p; }
    } catch { /* next port */ }
  }
  return null;
}

async function apiGet(port, path) {
  const r = await fetch(`http://127.0.0.1:${port}${path}`, { headers: { 'X-Hub-Token': TOKEN } });
  let j = null; try { j = await r.json(); } catch { /* non-json */ }
  return { status: r.status, json: j };
}

async function e11(port, o = {}) {
  const body = {
    route: o.route || 'B',
    plugin: o.plugin || 'demo3',
    func: o.func || 'hub_cmd',
    enc: o.enc || ENC,
    mode: o.mode || 'direct',
  };
  if (body.mode !== 'mem') {
    body.in_json = o.in_json !== undefined ? o.in_json
      : JSON.stringify({ command: o.command, args: o.args || {} });
  }
  if (o.timeoutMs) body.timeoutMs = o.timeoutMs;
  const t0 = Date.now();
  let r, j = null;
  try {
    r = await fetch(`http://127.0.0.1:${port}/api/v1/_e11`, {
      method: 'POST',
      headers: { 'X-Hub-Token': TOKEN, 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    try { j = await r.json(); } catch { /* keep null */ }
  } catch (e) {
    return { status: 0, json: null, ms: Date.now() - t0, neterr: String(e) };
  }
  return { status: r.status, json: j, ms: Date.now() - t0 };
}

/* ---------------- verdict plumbing ---------------- */
const results = [];
function logLine(rec) {
  mkdirSync(dirname(LOGF), { recursive: true });
  appendFileSync(LOGF, JSON.stringify(rec) + '\n');
}
function parseOut(j) {
  if (!j || typeof j.out_json !== 'string') return null;
  try { return JSON.parse(j.out_json); } catch { return null; }
}
async function settle(id, route, checks, notes, raw) {
  const fails = checks.filter((c) => !c.ok);
  const pass = fails.length === 0;
  const line = `[${pass ? 'PASS' : 'FAIL'}] ${id}${route ? ' route=' + route : ''}` +
    (fails.length ? ' :: ' + fails.map((f) => f.why).join('; ') : '') +
    (notes.length ? ' | ' + notes.join(' | ') : '');
  console.log(line);
  for (const c of checks) console.log(`    ${c.ok ? 'ok  ' : 'BAD '} ${c.what}`);
  for (const n of notes) console.log(`    note ${n}`);
  results.push({ id, route, pass });
  logLine({ ts: new Date().toISOString(), id, route: route || null, pass,
            fails: fails.map((f) => f.why), notes,
            status: raw?.status, channel: raw?.json?.channel,
            hub_ms: raw?.json?.ms, wall_ms: raw?.ms });
  return pass;
}
const chk = (ok, what) => ({ ok: !!ok, what });

/* ---------------- cases ---------------- */

async function caseEcho(port, route) {   /* e11-1 / e11-2 */
  const id = route === 'A' ? 'e11-2' : 'e11-1';
  const msg = `ping-${Date.now()}`;
  const r = await e11(port, { route, command: 'demo3.echo', args: { msg } });
  const out = parseOut(r.json);
  const notes = [];
  if (r.json && typeof r.json.ms === 'number') {
    notes.push(`in-hub ms=${r.json.ms}`);
    if (r.json.ms >= 50) notes.push('REVIEW: ms>=50 (docs threshold ms<50; machine load?)');
  }
  if (route === 'A') notes.push('VISUAL CHECK: Creo panel/menu still responsive after ProToolkitTaskExecute? (go/no-go for route A)');
  return settle(id, route, [
    chk(r.status === 200, `HTTP 200 (got ${r.status})`),
    chk(r.json && r.json.ok === true, 'envelope ok:true'),
    chk(r.json && r.json.channel === 'OK', `channel OK (got ${r.json && r.json.channel})`),
    chk(r.json && r.json.route === route, `route echoed = ${route}`),
    chk(r.json && r.json.function_return === 'NO_ERROR', `function_return NO_ERROR (got ${r.json && r.json.function_return})`),
    chk(out && out.ok === true, 'SDK out_json ok:true'),
    chk(out && out.result && out.result.echo === msg, 'echo payload byte-identical'),
    chk(out && out.result && out.result.bytes === Buffer.byteLength(msg), 'bytes = UTF-8 length'),
  ], notes, r);
}

async function caseUtf8(port, route) {  /* e11-3 */
  const msg = '网格导出✓';
  const r = await e11(port, { route, command: 'demo3.echo', args: { msg } });
  const out = parseOut(r.json);
  return settle('e11-3', route, [
    chk(r.status === 200 && r.json && r.json.ok === true && r.json.channel === 'OK', 'channel OK'),
    chk(out && out.ok === true && out.result && out.result.echo === msg, 'UTF-8 roundtrip lossless (网格导出✓)'),
    chk(out && out.result && out.result.bytes === Buffer.byteLength(msg, 'utf8'), 'bytes = UTF-8 length (13)'),
  ], [`enc=${ENC}`], r);
}

async function caseBig(port, route) {   /* e11-4 */
  const msg = 'E114' + 'x'.repeat(65536 - 4);   /* exactly 64KB ASCII */
  const r = await e11(port, { route, command: 'demo3.echo', args: { msg } });
  const out = parseOut(r.json);
  const notes = [`payload=${Buffer.byteLength(msg)} bytes`, `enc=${ENC}`];
  let echoed = out && out.result && typeof out.result.echo === 'string' ? out.result.echo : null;
  if (echoed && echoed !== msg) notes.push(`TRUNCATED/MUTATED: got ${echoed.length} bytes of ${msg.length} -> record actual ProArgument string threshold`);
  return settle('e11-4', route, [
    chk(r.status === 200 && r.json && r.json.ok === true && r.json.channel === 'OK', 'channel OK (dev BODY_MAX 256KB)'),
    chk(out && out.ok === true, 'SDK ok:true'),
    chk(echoed === msg, 'full 64KB roundtrip intact'),
    chk(out && out.result && out.result.bytes === 65536, 'bytes=65536'),
  ], notes, r);
}

async function caseMissingFunc(port, route) { /* e11-5 */
  const r = await e11(port, { route, func: 'no_such_fn', command: 'demo3.echo', args: { msg: 'x' } });
  const ch = r.json && r.json.channel;
  const wantB = 'PROCADDR_NULL';
  const okA = typeof ch === 'string' && ch.startsWith('TASK_');
  return settle('e11-5', route, [
    chk(r.status === 200, 'HTTP 200 (structured channel error, not transport error)'),
    chk(r.json && r.json.ok === false, 'envelope ok:false'),
    route === 'A'
      ? chk(okA, `channel TASK_* (docs expect TASK_E_NOT_FOUND; got ${ch})`)
      : chk(ch === wantB, `channel ${wantB} (got ${ch})`),
    chk(!r.json || r.json.out_json === undefined || r.json.out_json === null, 'no out_json (function never ran)'),
  ], [route === 'A' && ch && ch !== 'TASK_E_NOT_FOUND' ? `REVIEW: A channel=${ch}, docs expected TASK_E_NOT_FOUND - record` : null].filter(Boolean), r);
}

async function caseInactive(port) {     /* e11-6 */
  const want = opt('plugin', null);   /* S6 doc intent: unmount demo3 in panel, then --plugin demo3 */
  let victim = want ? { id: want, state: 'cli-specified' } : null;
  if (!victim) {
    const pl = await apiGet(port, '/api/v1/plugins');
    const pkgs = (pl.json && (pl.json.plugins || pl.json)) || [];
    const list = Array.isArray(pkgs) ? pkgs : [];
    victim = list.find((x) => x.id === 'demo2' && x.state !== 'ACTIVE');
    if (!victim) victim = list.find((x) => x.id !== 'demo3' && x.state !== 'ACTIVE' && x.state !== 'INVALID');
    if (!victim) victim = { id: 'demo2', state: '?' };
  }
  const r = await e11(port, { plugin: victim.id, command: 'demo3.echo', args: { msg: 'x' } });
  return settle('e11-6', null, [
    chk(r.status === 409, `HTTP 409 state-gate BEFORE channel (got ${r.status}; victim=${victim.id} state=${victim.state})`),
    chk(r.json && r.json.ok === false && r.json.error === 'plugin not active', 'error=plugin not active'),
    chk(r.json && r.json.channel === undefined, 'no channel field (gate fired pre-channel)'),
  ], ['state check precedes A/B channel selection, same order as future /call'], r);
}

async function caseFail(port, route) {  /* e11-7 */
  const r = await e11(port, { route, command: 'demo3.fail', args: {} });
  const out = parseOut(r.json);
  return settle('e11-7', route, [
    chk(r.status === 200 && r.json && r.json.ok === true, 'transport ok:true (channel healthy)'),
    chk(r.json && r.json.channel === 'OK', 'channel OK'),
    chk(out && out.ok === false, 'business out_json ok:false'),
    chk(out && out.err_code === 'P3_FAIL', 'err_code P3_FAIL passthrough'),
    chk(out && typeof out.hint === 'string' && out.hint.includes('E11-7'), 'hint passthrough (contains E11-7 marker)'),
  ], ['MANUAL: hub log [AUDIT] line for op=_e11 should contain the result fragment (err_code visible in result_json head)'], r);
}

async function caseSlow(port, route) {  /* e11-8 */
  const r = await e11(port, { route: route || 'B', command: 'demo3.slow', args: { sec: 5 }, timeoutMs: 15000 });
  const out = parseOut(r.json);
  return settle('e11-8', route || 'B', [
    chk(r.status === 200, 'HTTP 200 within timeoutMs=15000 envelope'),
    chk(r.json && r.json.ok === true && r.json.channel === 'OK', 'channel OK'),
    chk(out && out.ok === true && out.result && out.result.slept_sec === 5, 'slept_sec=5 (main thread really blocked)'),
    chk(r.json && typeof r.json.ms === 'number' && r.json.ms >= 4900, `in-hub ms>=4900 (got ${r.json && r.json.ms})`),
  ], ['RECORD: what happened to requests issued DURING the 5s block (503 busy vs queued)? -> docs\\23 9.2 E11-8 queue-semantics cell',
      'NOTE: manifest timeoutSec=2 for demo3.slow is a M3b /call-gate concern; _e11 bypasses gates by design'], r);
}

async function caseBoom(port) {         /* e11-9 */
  const name = `P3BOOM_${Date.now() % 1000000}`;
  const r = await e11(port, { route: 'B', command: 'demo3.boom', args: { name } });
  const out = parseOut(r.json);
  return settle('e11-9', 'B', [
    chk(r.status === 200 && r.json && r.json.ok === true && r.json.channel === 'OK', 'channel OK'),
    chk(out && out.ok === true && out.result && out.result.created === true, `feature created (name=${name})`),
  ], [(out && out.ok === false ? `BUSINESS FAIL: err_code=${out.err_code} err_msg=${out.err_msg || out.hint || ''}` : null),
      'VISUAL CHECK: model tree shows new import feature "' + name + '"; graphics regenerate; UI still responsive. Record visual result in docs\\23 9.2.'].filter(Boolean), r);
}

async function caseMem100(port, route) { /* e11-10 */
  const rt = route || 'B';
  const m0r = await e11(port, { mode: 'mem' });
  const m0 = m0r.json && m0r.json.mem_kb;
  const msg = 'mem100-' + 'y'.repeat(48);
  let errs = 0;
  for (let i = 0; i < 100; i++) {
    const r = await e11(port, { route: rt, command: 'demo3.echo', args: { msg } });
    const out = parseOut(r.json);
    if (!(r.json && r.json.ok === true && out && out.ok === true)) errs++;
  }
  const m1r = await e11(port, { mode: 'mem' });
  const m1 = m1r.json && m1r.json.mem_kb;
  const delta = (m0 && m1) ? (m1 - m0) / m0 * 100 : NaN;
  const notes = [`mem_kb first=${m0} last=${m1} delta=${isNaN(delta) ? '?' : delta.toFixed(2) + '%'}`,
                 `peak_kb=${m1r.json && m1r.json.peak_kb}`, `echo errors=${errs}/100`];
  if (!isNaN(delta) && delta > 5) notes.push('REVIEW: >5% (docs threshold +/-5%) - rerun mem100 before concluding leak');
  return settle('mem100', rt, [
    chk(errs === 0, '100/100 echoes clean'),
    chk(!isNaN(delta) && delta <= 5, 'working-set delta <= 5% after 100 echo roundtrips (release discipline)'),
  ], notes, m1r);
}

/* ---------------- groups / dispatch ---------------- */

const CASES = {
  'e11-1': (p) => caseEcho(p, 'B'),
  'e11-2': (p) => caseEcho(p, 'A'),
  'e11-3': (p) => dual(p, caseUtf8),
  'e11-4': (p) => dual(p, caseBig),
  'e11-5': (p) => dual(p, caseMissingFunc),
  'missing-func': (p) => dual(p, caseMissingFunc),
  'e11-6': (p) => caseInactive(p),
  'e11-7': (p) => dual(p, caseFail),
  'e11-8': (p) => caseSlow(p, ROUTE || 'B'),
  'e11-9': (p) => caseBoom(p),
  'boom': (p) => caseBoom(p),
  'e11-10': (p) => caseMem100(p, ROUTE),
  'mem100': (p) => caseMem100(p, ROUTE),
  'all-b': async (p) => {
    await caseEcho(p, 'B');
    await caseUtf8(p, 'B');
    await caseBig(p, 'B');
    await caseFail(p, 'B');
    await caseSlow(p, 'B');
    await caseMem100(p, 'B');
  },
  'all-a': async (p) => {
    await caseEcho(p, 'A');   /* e11-2 first: panel-responsive go/no-go */
    await caseUtf8(p, 'A');
    await caseBig(p, 'A');
    await caseFail(p, 'A');
  },
};

async function dual(port, fn) {
  if (ROUTE) return fn(port, ROUTE);
  await fn(port, 'B');
  await fn(port, 'A');
}

function usage() {
  console.log('cases: ' + Object.keys(CASES).join(' ') + ' list');
  console.log('flags: --case <id>  --route A|B (default: case-specific/dual)  --enc char|wchar (default char)  --plugin <id> (e11-6 victim override)');
}

if (CASE === 'list' || !CASES[CASE]) {
  usage();
  process.exit(CASE === 'list' ? 0 : 2);
}

const port = await findPort();
if (!port) {
  console.error('[FATAL] hub not reachable on 127.0.0.1:8765-8775.');
  console.error('        Checklist: Creo running? Aux Apps creo_hub started? dev DLL copied over creo_hub.dll (docs\\23 9.1 S0)?');
  process.exit(2);
}
console.log(`[i] hub on port ${port}, token loaded, enc=${ENC}`);

await CASES[CASE](port);

const failed = results.filter((r) => !r.pass);
console.log(`\n=== e11_probe: ${results.length - failed.length}/${results.length} PASS ===`);
console.log(`log: ${LOGF}`);
process.exit(failed.length ? 1 : 0);
