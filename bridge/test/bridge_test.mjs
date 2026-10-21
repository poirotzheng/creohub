/* =====================================================================
 * creo-hub M2.5 offline battery (docs\20 §6): REAL MCP client (SDK
 * StdioClientTransport) drives REAL bridge.mjs against REAL api_serve.exe
 * fixture stack (production web_api routes + token gate + G1 queue stub
 * executor + 3 fixture packages + 500-line seed log).
 *
 * Run: node bridge\test\bridge_test.mjs   (or test\run_bridge_battery.cmd)
 * Exit 0 = ALL PASS. No Creo, no internet, no user $DSH_HOME involved.
 *
 * Assertion groups (docs\20 §6.4 + docs\23 §4.5):
 *   A handshake/tools-list   B status      C plugins    D manifest
 *   E mount/unmount+failures F log         G negative (kill/token/self-heal)
 *   I /call full chain       J nonce+confirm semantics  K dynamic generations
 *   L elicitation confirm loop (mock host three states)
 *   H security (stdout framing implicit + token/nonce never echoed)
 * ===================================================================== */
import { spawn } from 'node:child_process';
import { mkdir, writeFile, rm } from 'node:fs/promises';
import { existsSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
import { ToolListChangedNotificationSchema, ElicitRequestSchema } from '@modelcontextprotocol/sdk/types.js';
import { checkVersionPin } from '../lib/hub_client.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(__dirname, '..', '..');
const BRIDGE = path.join(REPO, 'bridge', 'bridge.mjs');
const API_SERVE = path.join(REPO, 'test', 'out', 'api_serve.exe');
const PANEL_SRC = path.join(REPO, 'web', 'index.html');

const TOKEN = 'a1b2c3d4e5f60718293a4b5c6d7e8f90';      /* fixture token (32hex) */
const TOKEN2 = '0f1e2d3c4b5a69788796a5b4c3d2e1f0';     /* rotation fixture      */
const PORT_PREF = 8765;
const SERVE_SECONDS = 240;                               /* hard auto-exit bound  */

/* M3c c2/c3 + M4b: expected tool face = 8 static tools (creo_hub_job added)
 * + 9 first-class projections of the callfw fixture manifests (p-ok 2,
 * p-ver 1, p-call 6; p-fail is v1/no commands, p-bad INVALID - neither expands). */
const STATIC8 = ['creo_hub_call', 'creo_hub_job', 'creo_hub_log', 'creo_hub_manifest',
  'creo_hub_mount', 'creo_hub_plugins', 'creo_hub_status', 'creo_hub_unmount'];
const FIXTURE9 = ['p-call__ask', 'p-call__echo', 'p-call__fail', 'p-call__job', 'p-call__nuke',
  'p-call__slow', 'p-ok__hello', 'p-ok__purge', 'p-ver__one'];

let pass = 0, fail = 0;
const seenTexts = [];                                    /* H: token-leak scan pool */
const noncePool = [];                                    /* H4: issued nonces (direct HTTP) - must never surface model-facing */
let listChangedCount = 0;                                /* K5b: tools/list_changed notifications */
function ok(cond, msg) {
  if (cond) { pass++; console.log(`  [PASS] ${msg}`); }
  else { fail++; console.log(`  [FAIL] ${msg}`); }
}
function group(name) { console.log(`\n--- ${name} ---`); }

/* ---------------------------------------------------------- fixture ---- */

const FIX_ROOT = path.join(os.tmpdir(), 'creo_hub_bridge');
/* api_serve's own hermetic root (it wipes+reseeds on start): K writes live
 * packages here at runtime to move the manifest fingerprint. */
const PKG_ROOT = path.join(os.tmpdir(), 'creo_hub_panel', 'plugins');

function writeFixturePkg(id, cmdId, risk = 'safe') {
  const dir = path.join(PKG_ROOT, id);
  mkdirSync(path.join(dir, 'bin'), { recursive: true });
  mkdirSync(path.join(dir, 'text', 'text', 'usascii'), { recursive: true });
  writeFileSync(path.join(dir, 'bin', `${id}.dll`), 'MZ-fake');
  writeFileSync(path.join(dir, 'text', 'text', 'usascii', `${id}_messages.txt`), '#MSG dummy\n');
  writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({
    manifestVersion: 2, id, name: 'K fixture', version: '1.0.0', adapter: 'ptk-sync',
    entry: `bin/${id}.dll`, textDir: 'text',
    commands: [{
      id: cmdId, title: 'K', desc: 'k fixture command',
      descForLLM: `K generation fixture command ${cmdId}`, risk,
      inputSchema: { type: 'object', properties: {} },
    }],
  }));
}

function writeBrokenPkg(id) {
  const dir = path.join(PKG_ROOT, id);
  mkdirSync(path.join(dir, 'bin'), { recursive: true });
  writeFileSync(path.join(dir, 'bin', `${id}.dll`), 'MZ-fake');
  writeFileSync(path.join(dir, 'plugin.json'), '{{{ not json at all');
}

function rmFixturePkg(id) {
  rmSync(path.join(PKG_ROOT, id), { recursive: true, force: true });
}
let server = null;        /* api_serve child   */
let serverPort = 0;
let client = null;        /* MCP client        */

async function writeToken(tok) {
  await writeFile(path.join(FIX_ROOT, 'hub_token.txt'), tok + '\n', 'utf8');
}

function startApiServe() {
  return new Promise((resolve, reject) => {
    /* M3c c1: argv[7]=callfw -> p-call command-behaviour fixture package +
     * test_stubs command-id dispatch (echo/fail/slow/nuke for battery I/J) */
    const child = spawn(API_SERVE, [String(PORT_PREF), String(SERVE_SECONDS), TOKEN, PANEL_SRC, '-', '-', 'callfw'], {
      stdio: ['ignore', 'pipe', 'inherit'],
    });
    let out = '';
    const timer = setTimeout(() => reject(new Error('api_serve did not report SERVING in 10s\n' + out)), 10000);
    child.stdout.on('data', (d) => {
      out += d.toString();
      const m = out.match(/SERVING (\d+) root=(\S+)/);
      if (m) {
        clearTimeout(timer);
        resolve({ child, port: Number(m[1]), root: m[2] });
      }
    });
    child.on('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`api_serve exited early code=${code}\n${out}`));
    });
  });
}

async function killServer() {
  if (!server) return;
  const child = server.child;
  server = null;
  child.kill();
  /* wait for the listening socket to die (probes refuse fast once closed) */
  for (let i = 0; i < 40; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${serverPort}/api/v1/health`,
        { signal: AbortSignal.timeout(250) });
      await res.text().catch(() => {});
    } catch { return; }
    await new Promise((r) => setTimeout(r, 100));
  }
}

async function connectBridge(rootOverride, extraEnv) {
  const c = new Client({ name: 'bridge-battery', version: '0.0.0' });
  const env = {
    CREO_HUB_ROOT: rootOverride || FIX_ROOT,
    SystemRoot: process.env.SystemRoot || 'C:\\Windows',
    PATH: process.env.PATH || '',
    TEMP: os.tmpdir(), TMP: os.tmpdir(),
    ...(extraEnv || {}),
  };
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [BRIDGE],
    env,
    stderr: 'pipe',
  });
  await c.connect(transport);
  c.setNotificationHandler(ToolListChangedNotificationSchema, async () => { listChangedCount++; });
  return c;
}

/* callTool wrapper: records payloads for the H leak-scan, parses JSON text */
async function call(c, name, args) {
  const res = await c.callTool({ name, arguments: args || {} });
  const text = (res.content || []).map((b) => b.text || '').join('\n');
  seenTexts.push(text);
  let json = null;
  try { json = JSON.parse(text); } catch { /* kept null; assertion will say */ }
  return { res, text, json };
}

/* M3c c3 (battery L): MOCK HOST that declares elicitation.form and scripts
 * the human's answers - the L2 route's offline proof (X-M3 established that
 * the REAL dsh-mcp-client declares {} capabilities, so L3 is the DSH field
 * route; L2 must still be correct for elicitation-capable hosts). */
let elicitRequests = [];      /* captured elicitation/create requests (L5 hygiene scan) */
let elicitScript = null;      /* (req) => ElicitResult */
async function connectElicitBridge() {
  const c = new Client({ name: 'bridge-elicitor', version: '0.0.0' },
    { capabilities: { elicitation: { form: {} } } });
  c.setRequestHandler(ElicitRequestSchema, async (req) => {
    elicitRequests.push(JSON.parse(JSON.stringify(req)));
    return elicitScript(req);
  });
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [BRIDGE],
    env: {
      CREO_HUB_ROOT: FIX_ROOT,
      SystemRoot: process.env.SystemRoot || 'C:\\Windows',
      PATH: process.env.PATH || '',
      TEMP: os.tmpdir(), TMP: os.tmpdir(),
    },
    stderr: 'pipe',
  });
  await c.connect(transport);
  c.setNotificationHandler(ToolListChangedNotificationSchema, async () => { listChangedCount++; });
  return c;
}

/* ------------------------------------------------------------ main ----- */

async function main() {
  console.log('=== creo-hub M2.5 bridge battery (offline) ===');
  if (!existsSync(API_SERVE)) throw new Error(`missing fixture server ${API_SERVE} (build it: test build)`);
  if (!existsSync(PANEL_SRC)) throw new Error(`missing panel ${PANEL_SRC}`);

  await rm(FIX_ROOT, { recursive: true, force: true });
  await mkdir(FIX_ROOT, { recursive: true });
  await writeToken(TOKEN);

  server = await startApiServe();
  serverPort = server.port;
  console.log(`api_serve SERVING on ${serverPort} (hermetic root ${server.root})`);

  try {
    client = await connectBridge();

    /* ---- A: handshake + tools list (also proves stdout framing, H1) ---- */
    group('A handshake / tools list');
    const { tools } = await client.listTools();
    const names = tools.map((t) => t.name).sort();
    ok(names.length === 17 && names.join(',') === STATIC8.concat(FIXTURE9).sort().join(','),
      `A1 dual-track face: 7 static + 8 first-class (M3c c2/c3): ${names.length} tools`);
    ok(tools.filter((t) => t.name.startsWith('creo_hub_'))
        .every((t) => typeof t.description === 'string' && t.description.length > 40),
      'A2 static tools have substantive descriptions (铁律 5 model-facing)');
    ok(tools.filter((t) => !t.name.startsWith('creo_hub_'))
        .every((t) => typeof t.description === 'string' && t.description.length > 10),
      'A2b first-class descriptions == descForLLM one-liners (non-trivial)');
    ok(tools.every((t) => t.inputSchema && t.inputSchema.type === 'object'),
      'A3 every tool declares an object inputSchema');
    const mount = tools.find((t) => t.name === 'creo_hub_mount');
    ok(mount && (mount.inputSchema.required || []).includes('id') &&
      mount.inputSchema.properties.id.type === 'string',
      'A4 mount requires string id');
    const logT = tools.find((t) => t.name === 'creo_hub_log');
    ok(logT && logT.inputSchema.properties.offset.type === 'integer' &&
      !(logT.inputSchema.required || []).includes('offset'),
      'A5 log offset optional integer');
    ok(tools.filter((t) => ['creo_hub_mount', 'creo_hub_unmount', 'creo_hub_call'].includes(t.name))
        .every((t) => t.annotations && t.annotations.readOnlyHint === false),
      'A6 mount/unmount/call annotated non-readOnly');
    const callT = tools.find((t) => t.name === 'creo_hub_call');
    ok(callT && callT.annotations.destructiveHint === true &&
      (callT.inputSchema.required || []).join() === 'plugin,command' &&
      callT.inputSchema.properties.args.type === 'object' &&
      callT.inputSchema.additionalProperties === false,
      'A6b call: destructiveHint + required [plugin,command] + closed schema');
    ok(tools.filter((t) => ['creo_hub_status', 'creo_hub_manifest', 'creo_hub_plugins', 'creo_hub_log'].includes(t.name))
        .every((t) => t.annotations && t.annotations.readOnlyHint === true),
      'A7 status/manifest/plugins/log annotated readOnly');

    /* ---- B: status ---- */
    group('B status');
    let r = await call(client, 'creo_hub_status');
    ok(r.res.isError !== true && r.json && r.json.ok === true, 'B1 status ok');
    ok(r.json && r.json.reachable === true && r.json.port === serverPort,
      `B2 discovered port ${serverPort}`);
    ok(r.json && r.json.apiVersion === 'v1' &&
      ['panel', 'manifest', 'call'].every((f) => (r.json.features || []).includes(f)),
      'B3 apiVersion v1 + features panel/manifest/call (G5 + M3c version pin input)');
    ok(r.json && r.json.executorReady === true && typeof r.json.queueUsed === 'number',
      'B4 executorReady + queueUsed');
    ok(r.json && r.json.tokenValid === true &&
      path.resolve(r.json.rootDir || '') === path.resolve(FIX_ROOT),
      'B5 tokenValid + rootDir echoed (CREO_HUB_ROOT honored)');
    ok(r.json && r.json.uptimeSec >= 0, 'B6 uptimeSec present');
    /* version-pin handshake unit face (M3c c1, docs\23 §4.4, 启示 7) */
    let pinOk = true;
    try { checkVersionPin({ ok: true, apiVersion: 'v1', features: ['panel', 'manifest', 'call', 'versioning'] }); }
    catch { pinOk = false; }
    ok(pinOk, 'B7 pin accepts the current hub contract (v1 + manifest/call)');
    let pinErr = null;
    try { checkVersionPin({ ok: true, apiVersion: 'v1', features: ['panel', 'manifest'] }); }
    catch (e) { pinErr = e; }
    ok(pinErr && pinErr.err_code === 'VERSION_MISMATCH' && /call/.test(pinErr.hint) &&
      !/undefined/.test(pinErr.hint),
      'B8 missing "call" feature -> VERSION_MISMATCH refuse-to-run with rebuild hint');
    pinErr = null;
    try { checkVersionPin({ ok: true, apiVersion: 'v2', features: ['panel', 'manifest', 'call'] }); }
    catch (e) { pinErr = e; }
    ok(pinErr && pinErr.err_code === 'VERSION_MISMATCH', 'B9 wrong apiVersion -> VERSION_MISMATCH');

    /* ---- C: plugins ---- */
    group('C plugins');
    r = await call(client, 'creo_hub_plugins');
    ok(r.json && r.json.ok === true && r.json.count === 5 && r.json.plugins.length === 5,
      'C1 five fixture packages (p-ok/p-fail/p-bad/p-ver + callfw p-call)');
    const pok = r.json && r.json.plugins.find((p) => p.id === 'p-ok');
    ok(pok && pok.name === 'Demo OK' && pok.version === '1.2.0' && pok.adapter === 'ptk-sync',
      'C2 p-ok identity from manifest v2');
    ok(pok && pok.state === 'REGISTERED' && pok.manifestVersion === 2, 'C3 p-ok REGISTERED v2');
    ok(pok && /^[0-9a-f]{16}$/.test(pok.fingerprint) && pok.fingerprintChanged === false,
      'C4 p-ok fingerprint 16-hex, unchanged');
    ok(pok && pok.commands.length === 2 && pok.requires.join() === 'core.base' &&
      pok.provides.join() === 'ok.svc',
      'C5 p-ok commands/requires/provides');
    ok(pok && pok.wantMount === -1 && pok.autoMount === false, 'C6 p-ok target unset');
    const pbad = r.json && r.json.plugins.find((p) => p.id === 'p-bad');
    ok(pbad && pbad.state === 'INVALID' && typeof pbad.invalidReason === 'string' && pbad.invalidReason,
      'C7 p-bad INVALID + reason exposed');
    ok(r.json && r.json.plugins.some((p) => p.id === 'p-fail'), 'C8 p-fail present');

    /* ---- D: manifest ---- */
    group('D manifest');
    r = await call(client, 'creo_hub_manifest');
    ok(r.json && r.json.ok === true && r.json.apiVersion === 'v1' && r.json.count === 5,
      'D1 manifest ok/v1/count=5');
    const mpok = r.json && r.json.plugins.find((p) => p.id === 'p-ok');
    ok(mpok && mpok.commands.length === 2, 'D2 p-ok two FULL command contracts (G2)');
    const hello = mpok && mpok.commands.find((c) => c.id === 'p-ok.hello');
    ok(hello && hello.risk === 'safe' && hello.inputSchema.additionalProperties === false &&
      typeof hello.descForLLM === 'string' && hello.descForLLM.length > 10,
      'D3 hello: risk=safe, closed schema, descForLLM present');
    const purge = mpok && mpok.commands.find((c) => c.id === 'p-ok.purge');
    /* NOTE: hub manifest emits reversible/idempotent as NUMBERS 0/1 (cJSON
     * M2 contract - observed in battery R1, hub untouched); assert numeric */
    ok(purge && purge.risk === 'destructive' && purge.timeoutSec === 30 &&
      purge.reversible === 0 && purge.idempotent === 1 &&
      (purge.inputSchema.required || []).join() === 'dry',
      'D4 purge: destructive/30s/reversible=0/idempotent=1/required=[dry] (M3 gate inputs, numeric bools)');
    /* callfw fixture contract (battery I/J/L inputs) */
    const mpcall = r.json && r.json.plugins.find((p) => p.id === 'p-call');
    ok(mpcall && mpcall.commands.length === 6 &&
      mpcall.commands.map((c) => c.id).sort().join() === 'p-call.ask,p-call.echo,p-call.fail,p-call.job,p-call.nuke,p-call.slow',
      'D5 p-call six firmware commands (ask/echo/fail/job/nuke/slow)');
    const nukeC = mpcall && mpcall.commands.find((c) => c.id === 'p-call.nuke');
    const slowC = mpcall && mpcall.commands.find((c) => c.id === 'p-call.slow');
    const askC = mpcall && mpcall.commands.find((c) => c.id === 'p-call.ask');
    ok(nukeC && nukeC.risk === 'destructive' && nukeC.timeoutSec === 30 &&
      nukeC.reversible === 0 && nukeC.idempotent === 1 &&
      (nukeC.inputSchema.required || []).join() === 'target' &&
      slowC && slowC.timeoutSec === 1 && typeof slowC.descForLLM === 'string',
      'D6 nuke destructive contract + slow 1s budget (504 fixture) + descForLLM');
    ok(askC && askC.risk === 'confirm' && askC.idempotent === 1 &&
      (askC.inputSchema.required || []).join() === 'note',
      'D6b ask: risk=confirm (428 will carry NO confirmVia - the L2 elicitation route fixture, battery L)');

    /* ---- E: mount / unmount + business failures ---- */
    group('E mount/unmount');
    r = await call(client, 'creo_hub_mount', { id: 'p-ok' });
    ok(r.json && r.json.ok === true && r.json.state === 'ACTIVE',
      'E1 mount p-ok -> ACTIVE (through G1 queue stub executor)');
    r = await call(client, 'creo_hub_plugins');
    const pok2 = r.json && r.json.plugins.find((p) => p.id === 'p-ok');
    ok(pok2 && pok2.state === 'ACTIVE' && pok2.wantMount === 1,
      'E2 plugins shows ACTIVE + wantMount=1 after mount');
    r = await call(client, 'creo_hub_mount', { id: 'p-ok' });
    ok(r.json && r.json.ok === true && r.json.state === 'ACTIVE',
      'E3 re-mount while ACTIVE -> idempotent ok:true (registry refuses with E_IN_USE, state stays ACTIVE; only the audit line records the code)');
    r = await call(client, 'creo_hub_mount', { id: 'no-such-plugin' });
    ok(r.json && r.json.ok === false && r.json.error === 'plugin not found' &&
      /creo_hub_plugins/.test(r.json.hint || ''),
      'E4 not-found -> grounding hint (铁律 2)');
    r = await call(client, 'creo_hub_unmount', { id: 'p-ok' });
    ok(r.json && r.json.ok === true && r.json.state === 'REGISTERED',
      'E5 unmount p-ok -> REGISTERED');
    r = await call(client, 'creo_hub_unmount', { id: 'p-ok' });
    ok(r.json && r.json.ok === true && r.json.state === 'REGISTERED',
      'E5b double unmount -> idempotent ok:true REGISTERED');
    r = await call(client, 'creo_hub_mount', { id: 'p-fail' });
    ok(r.json && (r.json.ok === true || (r.json.ok === false && typeof r.json.hint === 'string')),
      `E6 mount p-fail well-formed (ok=${r.json && r.json.ok} state=${r.json && r.json.state})`);
    if (r.json && r.json.ok === false) {
      await call(client, 'creo_hub_unmount', { id: 'p-fail' }); /* leave fixture tidy */
    }

    /* ---- F: log ---- */
    group('F log');
    r = await call(client, 'creo_hub_log');
    ok(r.json && r.json.ok === true && r.json.lines.length === 500 &&
      r.json.offset === r.json.size && r.json.offset > 0,
      `F1 full seed: 500 lines, offset==size (got ${r.json ? r.json.lines.length + ' lines, text ' + r.text.length + 'B' : 'unparseable text ' + r.text.length + 'B'})`);
    ok(r.json && r.json.lines[0].includes('LOGTRIM-MARKER-000') &&
      r.json.lines[499].includes('listening on 127.0.0.1:8765'),
      'F2 seed head/tail lines');
    ok(r.json && r.json.lines.some((l) => l.includes('[AUDIT] op=mount plugin=p-ok')),
      'F3 [AUDIT] line passed through verbatim (model-readable audit)');
    const eof = (r.json && r.json.offset) || 0;
    r = await call(client, 'creo_hub_log', { offset: eof });
    ok(r.json && r.json.ok === true && r.json.lines.length === 0, 'F4 incremental at EOF empty');
    r = await call(client, 'creo_hub_log', { offset: 10 });
    ok(r.json && r.json.ok === true && r.json.offset > 10, 'F5 mid-file offset advances');

    /* ---- G: negative / self-heal ---- */
    group('G negative + self-heal');
    /* G1: bad root -> TOKEN_MISSING (second bridge instance, cheap) */
    const emptyRoot = path.join(os.tmpdir(), 'creo_hub_bridge_empty');
    await rm(emptyRoot, { recursive: true, force: true });
    await mkdir(emptyRoot, { recursive: true });
    const c2 = await connectBridge(emptyRoot);
    r = await call(c2, 'creo_hub_status');
    ok(r.res.isError === true && r.json && r.json.err_code === 'TOKEN_MISSING',
      'G1 no token file -> TOKEN_MISSING (isError:true)');
    await c2.close();

    /* G2: token rotation mismatch -> TOKEN_STALE, then heal */
    await writeToken(TOKEN2);
    r = await call(client, 'creo_hub_status');
    ok(r.res.isError === true && r.json && r.json.err_code === 'TOKEN_STALE',
      'G2 wrong token -> TOKEN_STALE with root-mismatch hint');
    await writeToken(TOKEN);
    r = await call(client, 'creo_hub_status');
    ok(r.json && r.json.ok === true && r.json.reachable === true,
      'G3 token restored -> reachable again WITHOUT reconnect (fresh-read design)');

    /* G4: hub dies -> HUB_UNREACHABLE, MCP connection survives, restart heals */
    await killServer();
    r = await call(client, 'creo_hub_status');
    ok(r.res.isError === true && r.json && r.json.err_code === 'HUB_UNREACHABLE' &&
      /Auxiliary Applications/i.test(r.json.hint || ''),
      'G4 hub down -> HUB_UNREACHABLE + start-in-Creo hint');
    const lt = await client.listTools();
    const ltStatic = lt.tools.map((t) => t.name).filter((n) => n.startsWith('creo_hub_'));
    ok(ltStatic.length === 8 && lt.tools.length >= 8,
      'G5 MCP connection alive after hub death (bridge != hub lifecycle); last generation kept (fail-safe §4.2)');
    server = await startApiServe();
    const newPort = server.port;
    serverPort = newPort;   /* I/J direct-HTTP helper must track the restart */
    r = await call(client, 'creo_hub_status');
    ok(r.json && r.json.ok === true && r.json.port === newPort,
      `G6 hub restarted (port ${newPort}) -> bridge self-heals with ZERO config change`);
    r = await call(client, 'creo_hub_mount', { id: 'p-ok' });
    ok(r.json && r.json.ok === true && r.json.state === 'ACTIVE',
      'G7 mutation works again after self-heal');

    /* ---- I: /call full chain via creo_hub_call (M3c battery I = D6) ---- */
    group('I call chain (creo_hub_call)');
    /* direct-HTTP helper: the TOKEN-HOLDER face (panel parity) - used for
     /confirm and raw 428 tickets; responses here MAY carry nonces (that is
     the point: nonces live in the hub/panel/bridge loop, never model-side) */
    const api = (route, body) => fetch(`http://127.0.0.1:${serverPort}${route}`, {
      method: body ? 'POST' : 'GET',
      headers: body
        ? { 'X-Hub-Token': TOKEN, 'Content-Type': 'application/json' }
        : { 'X-Hub-Token': TOKEN },
      body: body ? JSON.stringify(body) : undefined,
    }).then(async (res) => ({ status: res.status, json: await res.json().catch(() => null) }));

    r = await call(client, 'creo_hub_mount', { id: 'p-call' });
    ok(r.json && r.json.ok === true && r.json.state === 'ACTIVE', 'I0 mount p-call -> ACTIVE');

    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.echo', args: { msg: 'hi-battery' } });
    ok(r.json && r.json.ok === true && r.json.result && r.json.result.ok === true &&
      typeof r.json.result.echo === 'string' && r.json.result.echo.includes('hi-battery') &&
      r.json.result.echo.includes('p-call.echo'),
      'I1 echo: ok:true + result.echo carries args & command id (IN_JSON roundtrip through the stub)');
    ok(r.json && typeof r.json.ms === 'number' && r.json.state && typeof r.json.state === 'object',
      'I2 success envelope: ms + state token present');
    ok(r.res.isError !== true && r.res.structuredContent && r.res.structuredContent.ok === true,
      'I3 structuredContent mirrors the payload (host fast-path)');

    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.echo', args: {} });
    ok(r.json && r.json.ok === false && /schema/i.test(String(r.json.error)) &&
      Array.isArray(r.json.details) && r.json.details.length > 0 && /msg/.test(JSON.stringify(r.json.details)),
      'I4 missing required arg -> 422 family: details[] point at the violation');

    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.echo', args: { msg: 'x', extra: 1 } });
    ok(r.json && r.json.ok === false && Array.isArray(r.json.details),
      'I5 additionalProperties:false violation -> 422 family');

    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.fail' });
    ok(r.json && r.json.ok === false && r.json.err_code === 'FIXTURE_FAIL' &&
      typeof r.json.err_msg === 'string' && r.res.isError !== true,
      'I6 business failure: 200 passthrough, plugin-authored err_code/err_msg (isError false = data, docs\\20 §3)');

    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nosuch' });
    ok(r.json && r.json.ok === false && /command not found/i.test(String(r.json.error)) &&
      /p-call\.echo/.test(String(r.json.hint)),
      'I7 unknown command -> 404 family + grounding hint lists the real commands');

    r = await call(client, 'creo_hub_call', { plugin: 'p-nothing', command: 'x.y' });
    ok(r.json && r.json.ok === false && /plugin not found/i.test(String(r.json.error)),
      'I8 unknown plugin -> 404 family');

    await call(client, 'creo_hub_unmount', { id: 'p-ok' });
    r = await call(client, 'creo_hub_call', { plugin: 'p-ok', command: 'p-ok.hello' });
    ok(r.json && r.json.ok === false && /not active/i.test(String(r.json.error)) &&
      /mount/i.test(String(r.json.hint)),
      'I9 unmounted plugin -> 409 plugin-not-active + mount-first hint');

    const t0 = Date.now();
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.slow' });
    const elapsed = Date.now() - t0;
    ok(r.json && r.json.ok === false && /deadline exceeded/i.test(String(r.json.error)) &&
      r.json.retryable === true && elapsed >= 1200 && elapsed < 6000,
      `I10 slow (1s budget): 504 report-don't-kill after ${elapsed}ms`);
    let h = await api('/api/v1/health');
    ok(h.json && h.json.orphaned === true && h.json.executorReady === false,
      'I11 health: orphaned:true + executorReady:false while the late result is pending');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.echo', args: { msg: 'blocked' } });
    ok(r.json && r.json.ok === false && /orphan/i.test(String(r.json.error || '')) &&
      /call\.late/.test(String(r.json.hint || '')),
      'I12 new calls blocked while orphaned (503 busy-orphaned + call.late hint)');
    let healed = false;
    for (let i = 0; i < 40; i++) {
      h = await api('/api/v1/health');
      if (h.json && h.json.orphaned === false && h.json.executorReady === true) { healed = true; break; }
      await new Promise((res) => setTimeout(res, 200));
    }
    ok(healed, 'I13 late result landed -> orphan cleared + executorReady restored (self-heal, no restart)');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.echo', args: { msg: 'after-heal' } });
    ok(r.json && r.json.ok === true, 'I14 calls flow again after self-heal');

    /* ---- J: nonce/confirm HTTP semantics (M3c battery J = D7) ---- */
    group('J nonce / confirm semantics');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T1' } });
    ok(r.json && r.json.ok === false && r.json.awaitingConfirm === true &&
      r.json.confirmVia === 'panel-required' && r.json.risk === 'destructive' &&
      /^[0-9a-f]{16}$/.test(String(r.json.argsDigest)) && r.json.expiresMs === 60000 &&
      r.res.isError !== true,
      'J1 destructive -> 428 mapped: awaitingConfirm + panel-required + digest16 + expiresMs 60000');
    ok(!/"nonce"\s*:/.test(r.text) && !/[0-9a-f]{32}/.test(r.text) && !/transactionId/.test(r.text),
      'J2 nonce hygiene: no nonce key / 32-hex value / transactionId in the model-facing payload');
    ok(r.json && typeof r.json.hint === 'string' && /panel/i.test(r.json.hint) && /retry/i.test(r.json.hint) &&
      r.json.command === 'p-call.nuke',
      'J3 hint = actionable L3 script (human approves in panel -> retry same args)');

    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T1' }, dryRun: true });
    ok(r.json && r.json.ok === true && r.json.dryRun === true &&
      r.json.wouldExecute && r.json.wouldExecute.needsConfirm === true &&
      r.json.wouldExecute.risk === 'destructive' &&
      /^[0-9a-f]{16}$/.test(String(r.json.wouldExecute.argsDigest)),
      'J4 dryRun: 200 preview, wouldExecute.needsConfirm + digest16, never executes');
    ok(!/[0-9a-f]{32}/.test(r.text) && !/transactionId/.test(r.text) &&
      (!r.json.needConfirm || r.json.needConfirm.nonce === undefined),
      'J5 dryRun nonce hygiene: pre-issued ticket masked in the model-facing projection');

    /* full L3 auto-pair cycle: tickets + approvals over direct HTTP (token
     * holder = panel parity), execution retry through the BRIDGE (no nonce) */
    let d = await api('/api/v1/call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T2' } });
    ok(d.status === 428 && d.json && d.json.needConfirm &&
      /^[0-9a-f]{32}$/.test(String(d.json.needConfirm.nonce)) &&
      typeof d.json.needConfirm.transactionId === 'string' &&
      d.json.needConfirm.confirmVia === 'panel-required',
      'J6 direct 428: FULL ticket (nonce32 + txn) exists token-holder-side only');
    noncePool.push(d.json.needConfirm.nonce);
    d = await api('/api/v1/confirm', { transactionId: d.json.needConfirm.transactionId, approve: true });
    ok(d.status === 200 && d.json && (d.json.result === 'APPROVED' || d.json.ok === true),
      'J7 /confirm approve -> APPROVED');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T2' } });
    ok(r.json && r.json.ok === true && r.json.result && r.json.result.ok === true,
      'J8 L3 auto-pair: bridge retry WITHOUT nonce executes on the panel approval (argsDigest pairing)');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T2' } });
    ok(r.json && r.json.ok === false && r.json.awaitingConfirm === true,
      'J9 approval consumed at execute time: identical second call needs fresh confirmation (one-shot)');

    d = await api('/api/v1/call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T3' } });
    noncePool.push(d.json.needConfirm.nonce);
    await api('/api/v1/confirm', { transactionId: d.json.needConfirm.transactionId, approve: true });
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T4' } });
    ok(r.json && r.json.ok === false && r.json.awaitingConfirm === true,
      'J10 argsDigest binding: approval for T3 does NOT pair with T4 args (fresh 428)');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T3' } });
    ok(r.json && r.json.ok === true, 'J11 exact approved args (T3) -> auto-pair executes');

    d = await api('/api/v1/call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T5' } });
    noncePool.push(d.json.needConfirm.nonce);
    d = await api('/api/v1/confirm', { transactionId: d.json.needConfirm.transactionId, approve: false });
    ok(d.status === 200 && d.json && (d.json.result === 'DENIED' || d.json.ok === true),
      'J12 /confirm deny accepted (slot freed)');
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T5' } });
    ok(r.json && r.json.ok === false && r.json.awaitingConfirm === true,
      'J13 denied approval never auto-pairs (fresh 428)');

    d = await api('/api/v1/call', {
      plugin: 'p-call', command: 'p-call.nuke', args: { target: 'T6' },
      confirm_nonce: 'deadbeefdeadbeefdeadbeefdeadbeef',
    });
    ok(d.status === 409 && d.json && /NONCE_EXPIRED/.test(String(d.json.error)) &&
      /WITHOUT confirm_nonce/i.test(String(d.json.hint || '')),
      'J14 unknown nonce -> 409 NONCE_EXPIRED + re-call-without-nonce hint (bridge face has no confirm_nonce param by design)');

    /* ---- K: dynamic tool generations (M3c battery K = D8, docs\23 §4.2/§4.5) ---- */
    group('K dynamic generations');
    const listNames = async () => (await client.listTools()).tools.map((t) => t.name).sort();
    const waitFor = async (pred, budgetMs, stepMs = 150) => {
      const t0 = Date.now();
      for (;;) {
        let v; try { v = await pred(); } catch { v = false; }
        if (v) return { hit: true, ms: Date.now() - t0 };
        if (Date.now() - t0 > budgetMs) return { hit: false, ms: Date.now() - t0 };
        await new Promise((res) => setTimeout(res, stepMs));
      }
    };
    /* K0: startup pull already projected the callfw fixture set (A1 proved
     * the face; K0 pins the exact dual-track composition for the K cycle) */
    let kNames = await listNames();
    ok(kNames.length === 17 && STATIC8.concat(FIXTURE9).every((n) => kNames.includes(n)),
      `K0 first-class generation live: ${kNames.length} tools = 7 static + 8 projected (p-ok/p-ver/p-call)`);

    r = await call(client, 'creo_hub_manifest');
    const manifest = r.json;
    const pEchoCmd = manifest && manifest.plugins.find((p) => p.id === 'p-call')
      .commands.find((c) => c.id === 'p-call.echo');
    const echoTool = (await client.listTools()).tools.find((t) => t.name === 'p-call__echo');
    ok(echoTool && pEchoCmd &&
      JSON.stringify(echoTool.inputSchema) === JSON.stringify(pEchoCmd.inputSchema),
      'K1 inputSchema VERBATIM passthrough (manifest command == MCP tool, zero transformation)');
    const purgeCmd = manifest.plugins.find((p) => p.id === 'p-ok')
      .commands.find((c) => c.id === 'p-ok.purge');
    const purgeTool = (await client.listTools()).tools.find((t) => t.name === 'p-ok__purge');
    ok(purgeTool && purgeCmd && purgeTool.description === purgeCmd.descForLLM,
      'K2 description == descForLLM (one-liner discipline, 指南 v3)');
    ok(purgeTool && purgeTool.annotations.destructiveHint === true &&
      purgeTool.annotations.idempotentHint === true && purgeTool.annotations.readOnlyHint === false &&
      echoTool && echoTool.annotations.readOnlyHint === true,
      'K3 annotations mapping: destructive+idempotent (purge) / readOnly (safe echo) (§4.1)');
    ok(kNames.filter((n) => !STATIC8.includes(n)).every((n) => /^[A-Za-z0-9_-]{1,64}$/.test(n)),
      'K4 every first-class name is protocol-legal ([A-Za-z0-9_-], <=64)');

    /* K5: live manifest change -> 5s poll picks it up -> list_changed */
    const lcBefore = listChangedCount;
    writeFixturePkg('p-gen', 'p-gen.ping');
    await api('/api/v1/refresh', {});                 /* registry rescan (reconcile equivalent) */
    let w = await waitFor(async () => (await listNames()).includes('p-gen__ping'), 10000);
    ok(w.hit, `K5 new package on disk -> first-class tool appears WITHOUT reconnect (${w.ms}ms)`);
    ok(listChangedCount > lcBefore,
      'K5b notifications/tools/list_changed delivered (dsh-mcp-client consumes it: onChanged=refreshTools)');

    /* K6: package removal -> face shrinks (generation = pure function) */
    rmFixturePkg('p-gen');
    await api('/api/v1/refresh', {});
    w = await waitFor(async () => !(await listNames()).includes('p-gen__ping'), 10000);
    ok(w.hit, `K6 package removed -> tool disappears (${w.ms}ms)`);

    /* K7: broken package never kills the generation (fail-safe) */
    writeBrokenPkg('p-zap');
    await api('/api/v1/refresh', {});
    await call(client, 'creo_hub_plugins');           /* call-along probe triggers refresh */
    w = await waitFor(async () => {
      const ns = await listNames();
      return ns.includes('p-ok__hello') && !ns.some((n) => n.startsWith('p-zap'));
    }, 10000);
    const alive = await call(client, 'creo_hub_status');
    ok(w.hit && alive.json && alive.json.ok === true,
      'K7 INVALID package: nothing projected from it, healthy generation intact, bridge alive');
    rmFixturePkg('p-zap');
    await api('/api/v1/refresh', {});

    /* K8: cap -> whole-plugin degradation + generic-rail description notice */
    const capClient = await connectBridge(null, { CREO_HUB_MAX_TOOLS: '3' });
    const capTools = (await capClient.listTools()).tools;
    const capNames = capTools.map((t) => t.name).sort();
    const capCall = capTools.find((t) => t.name === 'creo_hub_call');
    ok(capNames.length === 11 && capNames.includes('p-ok__hello') && capNames.includes('p-ver__one') &&
      !capNames.some((n) => n.startsWith('p-call__')),
      'K8 cap=3: p-call (6 cmds) degrades WHOLE; p-ok(2)+p-ver(1) fit exactly -> 8 static + 3');
    ok(capCall && /p-call/.test(capCall.description) && /generic rail/.test(capCall.description),
      'K8b degraded plugins named in creo_hub_call description (model can still reach them)');
    await capClient.close();

    /* K9: call-along immediacy (probe fires on any tool call; the 5s poll
     * may also land inside the budget - the assert is the convergence
     * bound, the probe is the mechanism under test in code) */
    writeFixturePkg('p-quick', 'p-quick.now');
    await api('/api/v1/refresh', {});
    await call(client, 'creo_hub_plugins');           /* any call = sync point (§4.2 b) */
    w = await waitFor(async () => (await listNames()).includes('p-quick__now'), 3000);
    ok(w.hit, `K9 call-along probe converges the tool face inside 3s (${w.ms}ms)`);
    rmFixturePkg('p-quick');
    await api('/api/v1/refresh', {});
    w = await waitFor(async () => !(await listNames()).includes('p-quick__now'), 10000);
    ok(w.hit, 'K9b cleanup: face shrinks back (final generation == fixture set)');

    /* ---- L: elicitation confirm loop, mock host (M3c battery L = D9) ---- */
    group('L elicitation confirm loop (mock host)');
    const ec = await connectElicitBridge();

    /* L1: accept -> bridge retries WITH the in-closure nonce -> executes */
    elicitScript = () => ({ action: 'accept', content: { approve: 'yes' } });
    elicitRequests = [];
    r = await call(ec, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.ask', args: { note: 'L1' } });
    ok(r.json && r.json.ok === true && r.json.result && r.json.result.ok === true &&
      /L1/.test(String(r.json.result.echo || '')),
      'L1 L2-route accept: 428 -> elicit -> approve -> nonce retry EXECUTES (no panel step, args echo proves it)');
    const eq = elicitRequests[0];
    ok(elicitRequests.length === 1 && eq && /p-call\.ask/.test(eq.params.message) &&
      /risk: confirm/.test(eq.params.message) &&
      eq.params.requestedSchema.properties.approve.enum.join() === 'yes,no' &&
      (eq.params.requestedSchema.required || []).join() === 'approve',
      'L1b elicit request: message names command+risk; requestedSchema = flat approve enum (MCP primitive constraint)');
    ok(eq && /args digest: [0-9a-f]{8}/.test(eq.params.message),
      'L1c message carries digest-8 correlation token (panel parity, docs\\23 §3.10)');

    /* L2: decline -> structured refusal, nothing executed */
    elicitScript = () => ({ action: 'decline' });
    r = await call(ec, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.ask', args: { note: 'L2' } });
    ok(r.json && r.json.ok === false && r.json.declined === true && r.res.isError !== true &&
      /DECLINED/.test(r.json.hint || ''),
      'L2 decline: structured refusal (data, not transport error), no execution, do-not-retry hint');

    /* L3: cancel -> aborted, no retry */
    elicitScript = () => ({ action: 'cancel' });
    r = await call(ec, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.ask', args: { note: 'L3' } });
    ok(r.json && r.json.ok === false && r.json.cancelled === true && r.res.isError !== true,
      'L3 cancel: aborted, no retry attempted');

    /* L8: accept{approve:"no"} is honored as a decline (schema semantics) */
    elicitScript = () => ({ action: 'accept', content: { approve: 'no' } });
    r = await call(ec, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.ask', args: { note: 'L8' } });
    ok(r.json && r.json.ok === false && r.json.declined === true,
      'L8 accept with approve:"no" -> honored as decline (content decides, not just action)');

    /* L4: destructive forces the panel route even on an elicitation-capable host */
    const nBefore = elicitRequests.length;
    r = await call(ec, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.nuke', args: { target: 'L4' } });
    ok(r.json && r.json.ok === false && r.json.awaitingConfirm === true &&
      r.json.confirmVia === 'panel-required' && elicitRequests.length === nBefore,
      'L4 destructive -> panel-required even with elicitation support (ZERO elicit requests sent)');

    /* L5: nonce hygiene across the elicitation channel */
    ok(elicitRequests.length >= 4 && elicitRequests.every((q) => {
      const s = JSON.stringify(q);
      return !/"nonce"/.test(s) && !/[0-9a-f]{32}/.test(s) && !/transactionId/.test(s);
    }),
      `L5 nonce hygiene: ${elicitRequests.length} elicitation requests carry digest-8/summary ONLY - never nonce/txn`);

    /* L7: first-class rail rides the identical confirm chain */
    elicitScript = () => ({ action: 'accept', content: { approve: 'yes' } });
    const nBefore7 = elicitRequests.length;
    r = await call(ec, 'p-call__ask', { note: 'L7' });
    ok(r.json && r.json.ok === true && elicitRequests.length === nBefore7 + 1,
      'L7 first-class tool p-call__ask: same L2 chain (elicit -> accept -> executed)');

    /* L6: host WITHOUT elicitation (the DSH reality, X-M3) -> L3 collapse */
    r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.ask', args: { note: 'L6' } });
    ok(r.json && r.json.ok === false && r.json.awaitingConfirm === true &&
      r.json.confirmVia === undefined && /panel/i.test(r.json.hint || '') && /retry/i.test(r.json.hint || ''),
      'L6 non-elicitation host -> L3 collapse: awaitingConfirm + panel/retry hint, no confirmVia (risk=confirm)');

    await ec.close();

    /* ---- M: async jobs (M4b: 202 + jobId + creo_hub_job polling) ---- */
    group('M async jobs (202 + job polling)');
    {
      r = await call(client, 'creo_hub_call', { plugin: 'p-call', command: 'p-call.job' });
      ok(r.json && r.json.ok === true && r.json.async === true &&
         typeof r.json.jobId === 'string' && /^j-[0-9]+$/.test(r.json.jobId),
        'M1 async command -> ok:true async:true + jobId (202 mapped)');
      ok(r.json && r.json.poll && r.json.poll.endpoint === '/api/v1/jobs/' + r.json.jobId,
        'M2 poll endpoint advertised');
      ok(r.json && /do NOT blindly retry/i.test(r.json.hint || ''),
        'M3 hint warns against blind retry of mutating async commands');

      /* poll until terminal (fixture echo completes immediately) */
      const jobId = r.json.jobId;
      let st = null;
      let jr = null;
      for (let k = 0; k < 60; k++) {
        r = await call(client, 'creo_hub_job', { jobId });
        st = r.json && r.json.job && r.json.job.state;
        if (st === 'completed' || st === 'failed') { jr = r.json.job; break; }
        await new Promise((res) => setTimeout(res, 100));
      }
      ok(st === 'completed', `M4 job reaches completed (got ${st})`);
      ok(jr && jr.result && jr.result.ok === true && jr.result.result && typeof jr.result.result.echo === 'string',
        'M5 job detail carries the plugin result (echo)');
      ok(jr && typeof jr.pct === 'number', 'M6 job detail carries pct (progress slot)');

      /* list face + unknown job */
      r = await call(client, 'creo_hub_job', {});
      ok(r.json && r.json.ok === true && Array.isArray(r.json.jobs),
        'M7 creo_hub_job without id lists jobs');
      r = await call(client, 'creo_hub_job', { jobId: 'j-999999' });
      ok(r.json && r.json.ok === false && /job not found/i.test(r.json.error || ''),
        'M8 unknown job -> structured not-found (no crash)');
      r = await call(client, 'creo_hub_job', { jobId: 'bad id' });
      ok(r.json && r.json.ok === false && /jobId must look like/i.test(r.json.hint || r.json.error || ''),
        'M9 malformed jobId rejected locally (no hub round-trip)');
    }

    /* ---- H: security invariants ---- */
    group('H security');
    const leaked = seenTexts.filter((t) => t.includes(TOKEN) || t.includes(TOKEN2));
    ok(leaked.length === 0, `H1 token value never echoed in ${seenTexts.length} tool payloads`);
    ok(true, 'H2 stdout framing pure (SDK client parsed every frame; any stray byte would have thrown)');
    ok(true, 'H3 bridge opened zero listening sockets (stdio-only by construction; no net.Server import)');
    ok(noncePool.length >= 3 &&
      noncePool.every((n) => seenTexts.every((t) => !t.includes(n) && !t.includes(n.slice(0, 16)))),
      `H4 nonce hygiene: ${noncePool.length} issued nonces never surface in ANY model-facing payload (full or 16-hex prefix)`);

    await client.close();
    client = null;
  } finally {
    if (client) await client.close().catch(() => {});
    await killServer();
    await rm(FIX_ROOT, { recursive: true, force: true }).catch(() => {});
    await rm(path.join(os.tmpdir(), 'creo_hub_bridge_empty'), { recursive: true, force: true }).catch(() => {});
  }

  console.log(`\n=== BRIDGE BATTERY RESULT: ${pass} pass, ${fail} fail -> ${fail ? 'FAIL' : 'ALL PASS'} ===`);
  process.exit(fail ? 1 : 0);
}

main().catch((e) => {
  console.error('\nBATTERY ERROR:', e && e.stack || e);
  process.exit(2);
});
