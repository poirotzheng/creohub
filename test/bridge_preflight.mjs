/* =====================================================================
 * creo-hub M2.5 bridge PREFLIGHT (docs\21 §1 step 0)
 *
 * Double-click via test\run_bridge_preflight.cmd BEFORE restarting dsh
 * for the field round. Catches the foreseeable field failures early
 * (M2 lesson: wiring_check guard). Checks, in order:
 *   P1 node >= 18                 P2 bridge files present
 *   P3 MCP SDK installed          P4 user DSH patch applied + insert form
 *   P5 patch points at THIS repo  P6 hub live probe (token + port scan)
 *   P7 ask-gate wiring (ISSUE-B2 regression guard: a broken gate fails
 *      OPEN and is invisible from the usage side - hooks patch entry,
 *      sandbox-policy fallback pin + ACL temp assert pre-check, gate
 *      file contracts, live ask_gate.mjs run)
 *   P8 M3c call-face version pin (hub apiVersion=v1 + features include
 *      manifest/call, else the bridge refuse-to-runs) + P8b M3c bridge
 *      modules present (tool_gen.mjs, confirm.mjs)
 * Exit 0 = no FAIL (WARN allowed; hub-down is a WARN because Creo may
 * be started later), 1 = at least one FAIL (fix before field round).
 * Read-only: never writes outside this repo, never echoes the token.
 * ===================================================================== */
import { readFile, access } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(__dirname, '..');   /* this file lives in test\ */
const BRIDGE = path.join(REPO, 'bridge', 'bridge.mjs');
const HUBCLIENT = path.join(REPO, 'bridge', 'lib', 'hub_client.mjs');
const SDKPKG = path.join(REPO, 'bridge', 'node_modules', '@modelcontextprotocol', 'sdk', 'package.json');
const USER_PATCH = path.join(os.homedir(), '.dsh', 'cordis.patch.yml');
const TOKEN_FILE = path.join(REPO, 'hub_token.txt');

let nPass = 0, nWarn = 0, nFail = 0;
const PASS = (m) => { nPass++; console.log(`  [PASS] ${m}`); };
const WARN = (m) => { nWarn++; console.log(`  [WARN] ${m}`); };
const FAIL = (m) => { nFail++; console.log(`  [FAIL] ${m}`); };
const exists = (p) => access(p).then(() => true).catch(() => false);

console.log('=== creo-hub M2.5 bridge preflight ===');
console.log(`repo root: ${REPO}`);

/* P1 node version */
const major = Number(process.versions.node.split('.')[0]);
if (major >= 18) PASS(`P1 node ${process.versions.node} (>=18)`);
else FAIL(`P1 node ${process.versions.node} too old (need >=18)`);

/* P2 bridge files */
if (await exists(BRIDGE) && await exists(HUBCLIENT)) PASS('P2 bridge.mjs + lib/hub_client.mjs present');
else FAIL('P2 bridge files missing - restore from git (commit bf4b637+)');

/* P3 SDK */
if (await exists(SDKPKG)) {
  const pkg = JSON.parse(await readFile(SDKPKG, 'utf8'));
  if (pkg.version === '1.30.0') PASS(`P3 MCP SDK ${pkg.version} (= DSH-bundled version)`);
  else WARN(`P3 MCP SDK ${pkg.version} installed but pinned version is 1.30.0 (works, but keep in sync)`);
} else {
  FAIL('P3 MCP SDK missing - run: cd bridge && npm install');
}

/* P4 user patch applied with insert form */
let patchText = null;
if (await exists(USER_PATCH)) {
  patchText = await readFile(USER_PATCH, 'utf8');
  const hasEntry = patchText.includes('creohub-mcp') && patchText.includes('dsh-mcp-client');
  const hasInsert = /-\s*insert:\s*[\r\n]/.test(patchText);
  /* bare-id = TOP-LEVEL list item (column 0, no indent); entries nested
   * under "insert:" are indented and must NOT trip this (X-B1 semantics) */
  const bareId = /(^|[\r\n])-\s*id:\s*creohub-mcp/.test(patchText);
  if (hasEntry && hasInsert && !bareId) {
    PASS('P4 DSH patch applied with correct insert form');
  } else if (hasEntry && bareId && !hasInsert) {
    FAIL('P4 patch uses BARE "- id: creohub-mcp" form - that means modify-existing and will warn ' +
      '"patch: entry not found"; use deploy\\dsh_mcp_patch.yml (insert form) verbatim');
  } else if (hasEntry) {
    WARN('P4 patch mentions creohub-mcp but insert-form not detected - compare with deploy\\dsh_mcp_patch.yml');
  } else {
    FAIL(`P4 ${USER_PATCH} exists but has no creohub-mcp entry - append deploy\\dsh_mcp_patch.yml content`);
  }
} else {
  FAIL(`P4 ${USER_PATCH} not found - create it and paste deploy\\dsh_mcp_patch.yml content, then restart dsh`);
}

/* P5 patch points at THIS repo's bridge */
if (patchText && patchText.includes('creohub-mcp')) {
  const norm = (s) => s.replace(/\\\\/g, '\\').replace(/['"]/g, '').toLowerCase();
  if (norm(patchText).includes(norm(BRIDGE))) PASS(`P5 patch args point at this repo bridge (${BRIDGE})`);
  else WARN(`P5 patch does not reference ${BRIDGE} - if your repo lives elsewhere that is fine, otherwise fix args`);
  if (norm(patchText).includes(norm(REPO))) { /* CREO_HUB_ROOT ok */ }
  else WARN(`P5b patch env CREO_HUB_ROOT does not reference ${REPO} - token file will not be found at runtime`);
}

/* P6 hub live probe (WARN-only: Creo may not be running yet) */
let token = null;
let live = null;                 /* P8 consumes the discovered health body */
if (await exists(TOKEN_FILE)) {
  token = (await readFile(TOKEN_FILE, 'utf8')).trim();
  if (!/^[0-9a-fA-F]{16,128}$/.test(token)) { FAIL('P6 hub_token.txt exists but is not a hex token (content not shown)'); token = null; }
} else {
  WARN(`P6 ${TOKEN_FILE} missing - hub never started from this root? start creo_hub in Creo first`);
}
if (token) {
  const ports = [];
  for (let p = 8765; p <= 8775; p++) ports.push(p);
  const results = await Promise.all(ports.map(async (p) => {
    try {
      const res = await fetch(`http://127.0.0.1:${p}/api/v1/health`, {
        headers: { 'X-Hub-Token': token }, signal: AbortSignal.timeout(600),
      });
      if (res.status === 200) {
        const j = await res.json();
        if (j && j.ok === true && Array.isArray(j.features) && j.features.includes('panel')) return { p, j };
      }
      if (res.status === 401) return { p, unauthorized: true };
      return null;
    } catch { return null; }
  }));
  live = results.find((r) => r && r.j);
  if (live) {
    PASS(`P6 hub LIVE on 127.0.0.1:${live.p} (apiVersion=${live.j.apiVersion} features=${live.j.features.join('+')} ` +
      `executorReady=${live.j.executorReady} queueUsed=${live.j.queueUsed}) - token valid`);
    WARN('P6 battery G-group (token/hub-down semantics) needs the hub DOWN - full battery baseline 108/0 runs with hub stopped (live hub yields the one explainable red G4)');
  } else if (results.some((r) => r && r.unauthorized)) {
    FAIL('P6 a hub answered but REJECTED hub_token.txt - root mismatch (hub running from another root?)');
  } else {
    WARN('P6 hub unreachable on 8765..8775 - start Creo -> Auxiliary Applications -> creo_hub before the field round');
  }
}

/* P8 M3c call-face version pin (docs\23 §4.5): the bridge refuse-to-runs on
 * apiVersion != v1 or features missing manifest/call - preflight must catch
 * an OLD hub binary before the field round, not during it. WARN when the hub
 * is down (Creo may start later; P6 already reminded). */
if (live) {
  const feats = live.j.features || [];
  if (live.j.apiVersion === 'v1' && feats.includes('call') && feats.includes('manifest')) {
    PASS(`P8 hub call face pinned-compatible (apiVersion=v1, features include manifest+call) - creo_hub_call/first-class tools will work`);
  } else {
    FAIL(`P8 hub is TOO OLD for the M3c bridge (apiVersion=${live.j.apiVersion} features=${feats.join('+')}): ` +
      'the bridge will refuse to run (VERSION_MISMATCH). Rebuild+remount creo_hub.dll from THIS repo (M3b prod line), then restart Creo');
  }
} else {
  WARN('P8 call-face pin not verified (hub not running) - re-run preflight after Creo starts');
}
/* P8b M3c bridge module face complete */
{
  const mods = ['lib' + path.sep + 'tool_gen.mjs', 'lib' + path.sep + 'confirm.mjs'];
  const missing = [];
  for (const m of mods) if (!(await exists(path.join(REPO, 'bridge', m)))) missing.push(m);
  if (missing.length === 0) PASS('P8b bridge M3c modules present (tool_gen.mjs + confirm.mjs)');
  else FAIL(`P8b bridge modules missing: ${missing.join(', ')} - restore from git 2de4305`);
}

/* P7 ask-gate wiring (ISSUE-B2 regression guard: broken gate = silent fail-open) */
const GATE_HOOKS = path.join(REPO, 'deploy', 'hooks.json');
const GATE_SCRIPT = path.join(REPO, 'deploy', 'ask_gate.mjs');
/* same containment semantics as the Windows ACL runner's assertTempRootOutsideWorkspace */
const containsPath = (base, target) => {
  const rel = path.relative(base.toLowerCase(), target.toLowerCase());
  return rel === '' || !(rel === '..' || rel.startsWith('..' + path.sep) || path.isAbsolute(rel));
};

/* P7a hooks entry present in insert form */
if (patchText) {
  const has = patchText.includes('creohub-ask-gate') && patchText.includes('dsh-hooks-claude-code');
  const bare = /(^|[\r\n])-\s*id:\s*creohub-ask-gate/.test(patchText);
  if (has && !bare) PASS('P7a patch has creohub-ask-gate entry (insert form)');
  else if (has && bare) FAIL('P7a creohub-ask-gate is in BARE id form - new entries need "- insert:" (paste deploy\\dsh_hooks_patch.yml verbatim)');
  else FAIL('P7a patch has no creohub-ask-gate entry - append deploy\\dsh_hooks_patch.yml, then restart dsh');
} else {
  FAIL('P7a user patch unreadable - fix P4 first');
}

/* P7b sandbox-policy fallback pinned + ACL temp-outside-workspace assert pre-checked */
if (patchText) {
  const bare = /(^|[\r\n])-\s*id:\s*sandbox-policy/.test(patchText);
  const m = patchText.match(/workspaceRoot:\s*['"]?([A-Za-z]:[^'"\r\n]*)/);
  if (!bare) {
    FAIL('P7b patch has no bare "- id: sandbox-policy" modify entry - append deploy\\dsh_askgate_fix_patch.yml ' +
      '(without the pin, session-less hook shell calls fall back to the dsh LAUNCH dir; launching dsh from home ' +
      'trips the Windows ACL temp-outside-workspace assert, every hook is refused, and the gate fails OPEN)');
  } else if (!m) {
    FAIL('P7b sandbox-policy entry found but no workspaceRoot path value - compare with deploy\\dsh_askgate_fix_patch.yml');
  } else {
    const ws = m[1].trim();
    const tmp = os.tmpdir();
    if (!(await exists(ws))) FAIL(`P7b workspaceRoot ${ws} is not an existing directory (ACL runner requires it)`);
    else if (containsPath(ws, tmp)) FAIL(`P7b workspaceRoot ${ws} CONTAINS system temp ${tmp} - Windows ACL runner would refuse every hook (ISSUE-B2 layer-2 failure); pin another directory`);
    else PASS(`P7b sandbox-policy workspaceRoot pinned: ${ws} (system temp ${tmp} outside - ACL assert pre-checked)`);
  }
}

/* P7c gate file contracts (M3c c3: matcher is the ratified negative-lookahead
 * regex - assert SEMANTICS, not a literal substring: every mutating/first-class
 * tool gated, the four read-only tools released). */
if (await exists(GATE_HOOKS) && await exists(GATE_SCRIPT)) {
  try {
    const hj = JSON.parse(await readFile(GATE_HOOKS, 'utf8'));
    const matcher = (hj?.hooks?.PreToolUse ?? []).map((e) => e.matcher ?? '').join('|');
    let okMatcher = false;
    try {
      const re = new RegExp(matcher);
      const gated = ['mcp__creohub__creo_hub_mount', 'mcp__creohub__creo_hub_unmount',
        'mcp__creohub__creo_hub_call', 'mcp__creohub__p-ok__hello'];
      const released = ['mcp__creohub__creo_hub_status', 'mcp__creohub__creo_hub_plugins',
        'mcp__creohub__creo_hub_manifest', 'mcp__creohub__creo_hub_log'];
      okMatcher = gated.every((t) => re.test(t)) && released.every((t) => !re.test(t));
    } catch { okMatcher = false; }
    const gateSrc = await readFile(GATE_SCRIPT, 'utf8');
    const okScript = gateSrc.includes('permissionDecision') && gateSrc.includes('PreToolUse');
    if (okMatcher && okScript) PASS('P7c hooks.json matcher gates mount/unmount/call/first-class, releases readonly-4; ask_gate.mjs emits PreToolUse decision');
    else FAIL(`P7c gate contract broken (matcher ok=${okMatcher}, script ok=${okScript}) - restore from git 16023df`);
  } catch (e) {
    FAIL(`P7c cannot parse ${GATE_HOOKS}: ${e.message}`);
  }
} else {
  FAIL('P7c deploy\\hooks.json or deploy\\ask_gate.mjs missing - restore from git 16023df');
}

/* P7d live run of the gate script (WARN-skip when this shell's sandbox denies spawns) */
try {
  const { spawnSync } = await import('node:child_process');
  const payload = JSON.stringify({
    session_id: 'preflight', hook_event_name: 'PreToolUse', cwd: REPO,
    tool_name: 'mcp__creohub__creo_hub_mount', tool_input: { id: 'demo2' },
  });
  const r = spawnSync(process.execPath, [GATE_SCRIPT], { input: payload, encoding: 'utf8', timeout: 10000 });
  if (r.error) {
    WARN(`P7d gate live-run skipped (${r.error.code || r.error.message}) - double-click test\\run_bridge_preflight.cmd for the full check`);
  } else {
    let out = null;
    try { out = JSON.parse(r.stdout); } catch { /* handled below */ }
    const d = out?.hookSpecificOutput?.permissionDecision;
    if (r.status === 0 && d === 'ask') PASS('P7d gate live-run: ask_gate.mjs returned permissionDecision=ask');
    else FAIL(`P7d gate live-run bad output (exit=${r.status} decision=${String(d)}) stderr: ${(r.stderr || '').slice(0, 160)}`);
  }
} catch (e) {
  WARN(`P7d gate live-run skipped (${e.message})`);
}

console.log(`\n=== PREFLIGHT: ${nPass} pass, ${nWarn} warn, ${nFail} fail -> ${nFail ? 'NOT READY (fix FAILs)' : 'READY (WARNs are reminders)'} ===`);
process.exit(nFail ? 1 : 0);
