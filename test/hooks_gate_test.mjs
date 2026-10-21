#!/usr/bin/env node
/**
 * creo-hub ASK-GATE offline test (Q3 route A, docs\21 ISSUE-B2).
 *
 * Verifies deploy\hooks.json + deploy\ask_gate.mjs + deploy\dsh_hooks_patch.yml
 * against the REAL dsh-hook-protocol implementation (matchesMatcher /
 * matcherDiagnostic / parseHookOutput / mergeHookOutputs imported from the
 * installed DSH package tree) and runs the REAL hook script with the exact
 * PreToolUse payload shape dsh-hooks-claude-code sends (preToolPayload:
 * {session_id, transcript_path, cwd, hook_event_name, tool_name, tool_input,
 * tool_use_id}).
 *
 * Fully offline: no dsh process, no Creo, no hub, no network.
 * Run: node test\hooks_gate_test.mjs   (or double-click run_hooks_gate_test.cmd)
 * Env: DSH_PKG_ROOT overrides the @deepseek-ai package dir (default = the
 *      npx cache tree of the installed dsh on this machine).
 * Exit: 0 = all green, 1 = any failure.
 */
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
const HOOKS_JSON = join(REPO, 'deploy', 'hooks.json');
const GATE_MJS = join(REPO, 'deploy', 'ask_gate.mjs');
const PATCH_YML = join(REPO, 'deploy', 'dsh_hooks_patch.yml');
const PKG_ROOT =
  process.env.DSH_PKG_ROOT ||
  join(
    'C:', 'Users', '<USER>', 'AppData', 'Local', 'npm-cache', '_npx',
    '1e7f6d9597241db0', 'node_modules', '@deepseek-ai',
  );
const PROTOCOL_LIB = join(PKG_ROOT, 'dsh-hook-protocol', 'lib', 'index.js');

let pass = 0;
let fail = 0;
function ok(cond, label) {
  if (cond) { pass += 1; console.log(`[PASS] ${label}`); }
  else { fail += 1; console.log(`[FAIL] ${label}`); }
}
function section(title) { console.log(`--- ${title} ---`); }

/* ---------- load the real protocol implementation ---------- */
if (!existsSync(PROTOCOL_LIB)) {
  console.log(`[FAIL] Z0 dsh-hook-protocol lib found at ${PROTOCOL_LIB}`);
  console.log(`\n0/1 FAIL - set DSH_PKG_ROOT to your @deepseek-ai package dir`);
  process.exit(1);
}
const proto = await import(pathToFileURL(PROTOCOL_LIB).href);
const { matchesMatcher, matcherDiagnostic, parseHookOutput, mergeHookOutputs } = proto;
ok(typeof matchesMatcher === 'function' && typeof parseHookOutput === 'function' &&
   typeof mergeHookOutputs === 'function' && typeof matcherDiagnostic === 'function',
   'Z1 real dsh-hook-protocol imported (4 functions)');

/* ---------- A: hooks.json contract ---------- */
section('A hooks.json contract');
let cfg;
try {
  cfg = JSON.parse(readFileSync(HOOKS_JSON, 'utf8'));
  ok(true, 'A1 hooks.json parses as JSON');
} catch (e) {
  ok(false, `A1 hooks.json parses as JSON (${e.message})`);
}
const groups = cfg?.hooks?.PreToolUse;
ok(Array.isArray(groups) && groups.length === 1, 'A2 hooks.PreToolUse = 1 group (settings shape, bridge also accepts bare map)');
const group = groups?.[0];
ok(Array.isArray(group?.hooks) && group.hooks.length === 1 && group.hooks[0].type === 'command',
   'A3 group has exactly 1 command hook');
const matcher = group?.matcher;
/* M3c c3 (Q-M3c-2 ratified): negative-lookahead regex - gate EVERY
 * mcp__creohub__* tool except the four read-only ones (mount/unmount/
 * call + all dynamic first-class tools). Golden string; documented
 * fallback if this form ever breaks: explicit literal alternatives. */
const EXPECTED_MATCHER = 'mcp__creohub__(?!creo_hub_(status|plugins|manifest|log)$).+';
ok(matcher === EXPECTED_MATCHER,
   'A4 matcher == ratified negative-lookahead golden (gates call+first-class, releases readonly-4)');
ok(matcherDiagnostic(matcher, 'claude-code') === undefined, 'A5 matcherDiagnostic accepts the matcher (non-pure-literal -> unanchored regex semantics)');
const cmd = group?.hooks?.[0]?.command ?? '';
const cmdParts = cmd.split(/\s+/);
ok(cmdParts[0] === 'node', 'A6 command runs via node (PATH-proven by the MCP bridge entry)');
const gatePath = cmdParts.slice(1).join(' ');
ok(gatePath === GATE_MJS && existsSync(GATE_MJS), `A7 command points at the real script (${GATE_MJS})`);
ok(group?.hooks?.[0]?.timeout === 10, 'A8 hook timeout=10s set (hang backstop; failed/timed-out hook degrades to allow)');

/* ---------- B: matcher semantics against real tool names ---------- */
section('B matcher semantics (real matchesMatcher)');
const MUTATING = ['mcp__creohub__creo_hub_mount', 'mcp__creohub__creo_hub_unmount',
                  'mcp__creohub__creo_hub_call'];
const FIRST_CLASS = ['mcp__creohub__p-ok__hello', 'mcp__creohub__p-call__nuke',
                     'mcp__creohub__demo3__do_thing'];
const READONLY = ['mcp__creohub__creo_hub_status', 'mcp__creohub__creo_hub_plugins',
                  'mcp__creohub__creo_hub_manifest', 'mcp__creohub__creo_hub_log'];
ok(MUTATING.every((t) => matchesMatcher(matcher, t, 'claude-code') === true),
   'B1 all three static mutating tools match (incl. creo_hub_call, M3c)');
ok(FIRST_CLASS.every((t) => matchesMatcher(matcher, t, 'claude-code') === true),
   'B1b dynamic first-class tools match (any <plugin>__<cmd> tail is gated)');
ok(READONLY.every((t) => matchesMatcher(matcher, t, 'claude-code') === false),
   'B2 all four read-only tools do NOT match (stay direct)');
ok(['pwsh', 'read', 'write', 'mcp__other__mount'].every((t) => !matchesMatcher(matcher, t, 'claude-code')),
   'B3 unrelated tool names do NOT match (server prefix required, not substring)');

/* ---------- C: hook process end-to-end (real spawn, real parse) ---------- */
section('C hook e2e (spawnSync + real parseHookOutput/mergeHookOutputs)');
function preToolPayload(toolName) {
  // exact shape of dsh-hooks-claude-code preToolPayload(exec)
  return JSON.stringify({
    session_id: 'offline-test-session',
    transcript_path: '',
    cwd: REPO,
    hook_event_name: 'PreToolUse',
    tool_name: toolName,
    tool_input: { id: 'demo2' },
    tool_use_id: 'call_offline_1',
  }) + '\n';
}
function runGate(input) {
  return spawnSync('node', [GATE_MJS], { input, encoding: 'utf8', timeout: 8000 });
}
for (const tool of MUTATING.concat([FIRST_CLASS[0]])) {
  const r = runGate(preToolPayload(tool));
  ok(r.status === 0, `C1[${tool}] hook exits 0`);
  const out = parseHookOutput(r.status ?? undefined, r.stdout ?? '', r.stderr ?? '', 'PreToolUse');
  ok(out.decision === 'ask', `C2[${tool}] parseHookOutput decision = ask (hookEventName guard passed)`);
  ok(typeof out.reason === 'string' && out.reason.includes(tool),
     `C3[${tool}] reason carries the tool name from stdin payload`);
  const merged = mergeHookOutputs([out]);
  ok(merged.decision === 'ask' && merged.reason === out.reason,
     `C4[${tool}] mergeHookOutputs -> ask (bridge maps this to tools/pre-execute {kind:"ask"} -> approval popup)`);
}

/* ---------- D: fail-soft (a broken hook must still gate, never hang) ---------- */
section('D fail-soft');
const rEmpty = runGate('');
const outEmpty = parseHookOutput(rEmpty.status ?? undefined, rEmpty.stdout ?? '', rEmpty.stderr ?? '', 'PreToolUse');
ok(rEmpty.status === 0 && outEmpty.decision === 'ask', 'D1 empty stdin -> still exit 0 + ask (fallback tool label)');
const rGarbage = runGate('not json at all\n');
const outGarbage = parseHookOutput(rGarbage.status ?? undefined, rGarbage.stdout ?? '', rGarbage.stderr ?? '', 'PreToolUse');
ok(rGarbage.status === 0 && outGarbage.decision === 'ask', 'D2 garbage stdin -> still exit 0 + ask');
ok((rEmpty.stdout ?? '').trim().startsWith('{') && (rGarbage.stdout ?? '').trim().startsWith('{'),
   'D3 stdout is a single JSON object (codec only parses stdout starting with "{")');

/* ---------- D13: negative-lookahead matcher edge semantics (docs\23 §4.3 风险点,
 * Q-M3c-2 ratified form; real matchesMatcher, not a local re-implementation) ---- */
section('D13 negative-lookahead edges (real protocol)');
ok(matchesMatcher(matcher, 'mcp__creohub__creo_hub_status_extra', 'claude-code') === true,
   'D13a lookalike suffix (status_extra) IS gated - the $ anchor makes the readonly exemption exact');
ok(matchesMatcher(matcher, 'mcp__creohub__creo_hub_log2', 'claude-code') === true,
   'D13b log2 IS gated (not in the readonly exemption list)');
ok(['mcp__creohubx__creo_hub_mount', 'mcp__creohub__'].every((t) =>
     matchesMatcher(matcher, t, 'claude-code') === false),
   'D13c wrong server prefix / empty tail do NOT match (.+ requires one char after the prefix)');
ok(matchesMatcher(matcher, 'mcp__creohub__x', 'claude-code') === true,
   'D13d minimal first-class tail IS gated');

/* ---------- E: patch snippet shape (X-B1 lesson: insert syntax) ---------- */
section('E dsh_hooks_patch.yml shape');
let patchText = '';
try {
  patchText = readFileSync(PATCH_YML, 'utf8');
  ok(true, 'E1 patch snippet readable');
} catch (e) {
  ok(false, `E1 patch snippet readable (${e.message})`);
}
ok(/^- insert:/m.test(patchText), 'E2 top-level "- insert:" syntax (new entry, X-B1)');
ok(!/^- id:/m.test(patchText), 'E3 no bare top-level "- id:" (that would only MODIFY an existing entry)');
ok(patchText.includes("name: '@deepseek-ai/dsh-hooks-claude-code'"), 'E4 mounts the shipped hooks bridge package');
const cfgPathMatch = patchText.match(/configPath:\s*'([^']+)'/);
ok(!!cfgPathMatch, 'E5 configPath present');
if (cfgPathMatch) {
  const p = cfgPathMatch[1];
  ok(/^[A-Za-z]:\\/.test(p), 'E6 configPath is absolute (relative would resolve from the dsh launch dir)');
  ok(resolve(p) === HOOKS_JSON && existsSync(p), 'E7 configPath points at deploy\\hooks.json in this repo');
}

/* ---------- summary ---------- */
console.log('');
console.log(`${pass + fail} checks: ${pass} PASS, ${fail} FAIL`);
if (fail > 0) {
  console.log('ASK-GATE NOT READY - fix every [FAIL] line above');
  process.exit(1);
}
console.log('ASK-GATE READY - apply deploy\\dsh_hooks_patch.yml to cordis.patch.yml, restart dsh, retest gated tools (mount/unmount/call/first-class)');
process.exit(0);
