/* =====================================================================
 * creo-hub M3c c2 CONTRACT TEST (DoD D10, docs\23 §4.5 - SolidPilot
 * drift-test style): the bridge's first-class tool face must match the
 * hub manifest FIELD-BY-FIELD, three ways at once:
 *
 *   1. live passthrough  - genTools() output vs the LIVE /api/v1/manifest
 *      of the callfw fixture firmware (description/inputSchema/annotations
 *      re-derived INDEPENDENTLY in this file - no tool_gen internals);
 *   2. golden snapshot   - test\fixtures\expected_first_class_tools.json
 *      (committed): catches drift on BOTH sides (C fixture manifests in
 *      api_serve.c  <->  bridge projection in lib\tool_gen.mjs);
 *   3. unit contracts    - naming/sanitize/truncation/cap/degrade rules
 *      asserted from the spec text, not from the implementation.
 *
 * The fixture source of truth is api_serve.c (mkpkg callfw set) - the
 * SAME fixtures manifest_lint's C face validates; nothing is duplicated
 * into this test except the golden projection.
 *
 * Run: node test\tool_contract_test.mjs [--update-golden]
 *      (or test\run_tool_contract_test.cmd - self-compiles api_serve)
 * Exit 0 = ALL PASS. Offline, no Creo, no internet.
 * ===================================================================== */
import { spawn } from 'node:child_process';
import { mkdir, writeFile, rm, readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import {
  genTools, makeRawName, sanitizeShort, annotationsFor, fnvHex4, MCP_NAME_MAX,
} from '../bridge/lib/tool_gen.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(__dirname, '..');
const API_SERVE = path.join(REPO, 'test', 'out', 'api_serve.exe');
const PANEL_SRC = path.join(REPO, 'web', 'index.html');
const GOLDEN = path.join(REPO, 'test', 'fixtures', 'expected_first_class_tools.json');

const TOKEN = 'a1b2c3d4e5f60718293a4b5c6d7e8f90';
const PORT_PREF = 8766;           /* distinct from battery's 8765 */
const SERVE_SECONDS = 90;
const FIX_ROOT = path.join(os.tmpdir(), 'creo_hub_contract');
const UPDATE_GOLDEN = process.argv.includes('--update-golden');

let pass = 0, fail = 0;
function ok(cond, msg) {
  if (cond) { pass++; console.log(`  [PASS] ${msg}`); }
  else { fail++; console.log(`  [FAIL] ${msg}`); }
}
/* ------------------------------------------------------------ fixture -- */
let server = null;
function startApiServe() {
  return new Promise((resolve, reject) => {
    const child = spawn(API_SERVE,
      [String(PORT_PREF), String(SERVE_SECONDS), TOKEN, PANEL_SRC, '-', '-', 'callfw'],
      { stdio: ['ignore', 'pipe', 'inherit'] });
    let out = '';
    const timer = setTimeout(() => reject(new Error('api_serve SERVING timeout\n' + out)), 10000);
    child.stdout.on('data', (d) => {
      out += d.toString();
      const m = out.match(/SERVING (\d+) root=(\S+)/);
      if (m) { clearTimeout(timer); resolve({ child, port: Number(m[1]) }); }
    });
    child.on('exit', (code) => { clearTimeout(timer); reject(new Error(`api_serve exited code=${code}\n${out}`)); });
  });
}

async function main() {
  console.log('=== creo-hub M3c tool-face CONTRACT TEST (D10) ===');
  if (!existsSync(API_SERVE)) throw new Error(`missing ${API_SERVE} (run test\\run_tool_contract_test.cmd)`);
  await rm(FIX_ROOT, { recursive: true, force: true });
  await mkdir(FIX_ROOT, { recursive: true });
  await writeFile(path.join(FIX_ROOT, 'hub_token.txt'), TOKEN + '\n', 'utf8');

  server = await startApiServe();
  const port = server.port;
  const apiGet = async (route) => {
    const res = await fetch(`http://127.0.0.1:${port}${route}`, { headers: { 'X-Hub-Token': TOKEN } });
    return { status: res.status, json: await res.json().catch(() => null) };
  };

  try {
    const { status, json: manifest } = await apiGet('/api/v1/manifest');
    ok(status === 200 && manifest && manifest.ok === true && manifest.apiVersion === 'v1',
      'T1 live manifest endpoint (callfw firmware, same fixtures as manifest_lint C face)');

    const gen = genTools(manifest, { maxTools: 32 });

    /* --- T2: per-tool live passthrough, independently re-derived --- */
    const cmds = [];
    for (const pkg of (manifest.plugins || [])) {
      if (pkg.manifestVersion !== undefined && pkg.manifestVersion !== 2) continue;
      for (const c of (Array.isArray(pkg.commands) ? pkg.commands : [])) cmds.push({ pkg, c });
    }
    const expandable = cmds.filter(({ c }) => c && c.inputSchema && c.inputSchema.type === 'object');
    ok(gen.tools.length === expandable.length && expandable.length === 9,
      `T2 coverage: every expandable manifest command projects exactly one tool (${gen.tools.length}/9)`);

    let passthrough = true, firstBad = '';
    for (const t of gen.tools) {
      const src = expandable.find(({ pkg, c }) => pkg.id === t.plugin && c.id === t.command);
      if (!src) { passthrough = false; firstBad = `${t.name}: no manifest source`; break; }
      const { c } = src;
      /* independent re-derivation (spec text, NOT tool_gen internals): */
      const risk = String(c.risk || 'safe').toLowerCase();
      const wantAnn = { openWorldHint: false };
      if (risk === 'safe') wantAnn.readOnlyHint = true;
      else {
        wantAnn.readOnlyHint = false;
        if (risk === 'destructive' || risk === 'confirm') wantAnn.destructiveHint = true;
      }
      if (c.idempotent === true || c.idempotent === 1) wantAnn.idempotentHint = true;
      const short = c.id.startsWith(t.plugin + '.') ? c.id.slice(t.plugin.length + 1) : c.id;
      const wantName = `${t.plugin}__${short.replace(/[^A-Za-z0-9_]/g, '_')}`;
      if (t.name !== wantName) { passthrough = false; firstBad = `${t.name}: name != ${wantName}`; break; }
      if (t.description !== c.descForLLM) { passthrough = false; firstBad = `${t.name}: description != descForLLM`; break; }
      if (JSON.stringify(t.inputSchema) !== JSON.stringify(c.inputSchema)) { passthrough = false; firstBad = `${t.name}: inputSchema not verbatim`; break; }
      if (JSON.stringify(t.annotations) !== JSON.stringify(wantAnn)) { passthrough = false; firstBad = `${t.name}: annotations ${JSON.stringify(t.annotations)} != ${JSON.stringify(wantAnn)}`; break; }
      if (t.title !== c.id) { passthrough = false; firstBad = `${t.name}: title != command id`; break; }
    }
    ok(passthrough, `T3 field-by-field passthrough vs LIVE manifest (name/description/inputSchema/annotations/title)${firstBad ? ' -- ' + firstBad : ''}`);

    /* --- T4: golden snapshot (three-way drift detector) --- */
    const projection = gen.tools
      .map((t) => ({ name: t.name, title: t.title, description: t.description, annotations: t.annotations, inputSchema: t.inputSchema }))
      .sort((a, b) => (a.name < b.name ? -1 : 1));
    if (UPDATE_GOLDEN) {
      await mkdir(path.dirname(GOLDEN), { recursive: true });
      await writeFile(GOLDEN, JSON.stringify(projection, null, 2) + '\n', 'utf8');
      console.log(`  [INFO] golden written: ${GOLDEN} (${projection.length} tools)`);
      ok(true, 'T4 golden snapshot UPDATED (--update-golden)');
    } else {
      let golden = null;
      try { golden = JSON.parse(await readFile(GOLDEN, 'utf8')); } catch { /* missing */ }
      let gDiff = '';
      if (!Array.isArray(golden) || golden.length !== projection.length) {
        gDiff = `count ${(golden || []).length} != ${projection.length}`;
      } else {
        for (let i = 0; i < projection.length; i++) {
          if (JSON.stringify(golden[i]) !== JSON.stringify(projection[i])) {
            gDiff = `tool[${i}] ${projection[i].name}: golden ${JSON.stringify(golden[i]).slice(0, 120)} != live ${JSON.stringify(projection[i]).slice(0, 120)}`;
            break;
          }
        }
      }
      ok(gDiff === '', `T4 golden snapshot matches (committed projection == live genTools output)${gDiff ? ' -- ' + gDiff : ''}`);
    }

    /* --- T5: cap determinism (spec: whole-plugin degrade, greedy in manifest order) --- */
    const capped = genTools(manifest, { maxTools: 3 });
    ok(capped.tools.map((t) => t.name).sort().join(',') === 'p-ok__hello,p-ok__purge,p-ver__one' &&
      capped.degraded.some((d) => d.plugin === 'p-call' && d.reason === 'cap' && d.commands === 6),
      'T5 cap=3: p-call(6) whole-degrades with reason; p-ok(2)+p-ver(1) fit (order-independent for this set)');

    /* --- T6: truncation + FNV suffix (spec §4.1: >64 -> truncate + 4-hex) --- */
    const longPid = 'pluginwithaveryverylongidentifier';
    const longCmd = longPid + '.command_with_ridiculously_long_name_to_force_overflow';
    const tn = makeRawName(longPid, longCmd);
    ok(tn.length <= MCP_NAME_MAX && tn.endsWith('_' + fnvHex4(longCmd)) &&
      tn === makeRawName(longPid, longCmd),
      `T6 truncation: ${tn.length}<=64 chars, ends with FNV-4hex of the full command id, deterministic`);

    /* --- T7: sanitize rule --- */
    ok(makeRawName('p-x', 'p-x.weird-name.v2') === 'p-x__weird_name_v2' &&
      sanitizeShort('a.b-c/d') === 'a_b_c_d',
      'T7 sanitize: cmd_short = id minus plugin prefix, [^A-Za-z0-9_] -> _');

    /* --- T8: defensive schema degrade (per-command, generic rail only) --- */
    const noSchema = genTools({ plugins: [{ id: 'q', manifestVersion: 2, commands: [{ id: 'q.a', risk: 'safe' }] }] });
    ok(noSchema.tools.length === 0 &&
      noSchema.degraded.some((d) => d.plugin === 'q' && d.reason === 'schema:q.a'),
      'T8 missing/non-object schema -> command does NOT expand, degraded reason recorded');

    /* --- T9: numeric-bool annotations (C cJSON emits 0/1) --- */
    const a1 = annotationsFor({ risk: 'safe', idempotent: 1 });
    const a2 = annotationsFor({ risk: 'destructive', reversible: 0 });
    ok(a1.idempotentHint === true && a1.readOnlyHint === true &&
      a2.destructiveHint === true && a2.readOnlyHint === false && a2.idempotentHint === undefined &&
      a1.openWorldHint === false && a2.openWorldHint === false,
      'T9 numeric bools (idempotent:1) map same as true; destructive -> destructiveHint, never readOnly');

    console.log(`\n=== TOOL CONTRACT RESULT: ${pass} pass, ${fail} fail -> ${fail ? 'FAIL' : 'ALL PASS'} ===`);
  } finally {
    if (server) server.child.kill();
  }
  process.exit(fail ? 1 : 0);
}

main().catch((e) => { console.error('[contract] fatal: ' + ((e && e.stack) || e)); if (server) server.child.kill(); process.exit(2); });
