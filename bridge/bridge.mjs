#!/usr/bin/env node
/* =====================================================================
 * creo-hub MCP bridge (docs\20, M3c docs\23 §4) - stdio server:
 * seven management tools (static face) + dynamic first-class command
 * tools projected from the live manifest (generation swap on fingerprint
 * change, notifications/tools/list_changed).
 *
 *   mcp host (DSH / Claude / Cursor)  <-stdio JSON-RPC->  this process
 *   this process  <-HTTP 127.0.0.1:8765..8775 + X-Hub-Token->  creo_hub.dll
 *
 * Invariants:
 *   - STATELESS: no hub data cached between calls; every call re-discovers
 *     port and re-reads the token file (rotation-safe, crash-safe);
 *   - stdout carries ONLY MCP JSON-RPC frames (polluting stdout breaks
 *     framing - dsh-sdk-minimal README warning); diagnostics -> stderr;
 *   - the token value is never logged, never returned, never echoed in
 *     errors (only presence/validity);
 *   - zero listening sockets.
 * ===================================================================== */
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import { BridgeError, discover, get, getStatus, post, postCall, resolveRoot } from './lib/hub_client.mjs';
import { genTools, DEFAULT_MAX_TOOLS } from './lib/tool_gen.mjs';
import { confirmFlow, projectPanelWait } from './lib/confirm.mjs';

const TEXT_CAP = 65536; /* text projection cap; overflow -> valid digest JSON */
const MAX_TOOLS_ENV = 'CREO_HUB_MAX_TOOLS';
const GEN_POLL_MS = 5000;        /* §4.2 trigger (c): light health poll, <1KB */
const GEN_PROBE_THROTTLE_MS = 250; /* §4.2 trigger (b): call-along sync point.
                                    * Health GET is ~1ms on loopback; 250ms caps
                                    * the probe at 4/s worst case while keeping
                                    * back-to-back call sequences immediate. */

/* ------------------------------------------------------------ tools ---- */

const TOOLS = [
  {
    name: 'creo_hub_status',
    title: 'Creo hub status',
    description:
      'Diagnose connectivity to the creo-hub plugin manager running inside Creo on THIS machine. ' +
      'Returns {reachable, port, apiVersion, features, uptimeSec, executorReady, queueUsed, tokenValid, rootDir}. ' +
      'ALWAYS call this first when any other creo_hub_* tool fails with HUB_UNREACHABLE/TOKEN_* - it tells you ' +
      'whether Creo+hub are running, which port was discovered, and whether hub_token.txt matches. Read-only.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    annotations: { readOnlyHint: true, openWorldHint: false },
    handler: async () => {
      /* pin:false - the diagnostic tool must be able to REPORT a
       * version-mismatched hub instead of dying on it (M3c c1) */
      const { port, health, root } = await discover({ pin: false });
      return {
        ok: true,
        reachable: true,
        port,
        apiVersion: health.apiVersion,
        hubVersion: health.hubVersion,
        features: health.features,
        uptimeSec: health.uptimeSec,
        executorReady: health.executorReady,
        orphaned: health.orphaned === true,
        queueUsed: health.queueUsed,
        jobs: health.jobs,
        tokenValid: true,
        rootDir: root,
      };
    },
  },
  {
    name: 'creo_hub_manifest',
    title: 'Creo hub manifest (single source of truth)',
    description:
      'Full plugin manifest registry of creo-hub: every package with id/name/version/state/fingerprint and ' +
      'COMPLETE command contracts (risk, descForLLM, inputSchema, timeoutSec, reversible, idempotent). ' +
      'This is the single source of truth for what plugins CAN do - read it instead of trusting memory; ' +
      'descriptors may have changed on disk (fingerprintChanged=true means manifest-on-disk differs from ' +
      'the mounted build; only Unmount+Mount clears it). Command contracts here feed creo_hub_call ' +
      '(exact command ids + inputSchema). Read-only.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    annotations: { readOnlyHint: true, openWorldHint: false },
    handler: async () => get('/api/v1/manifest'),
  },
  {
    name: 'creo_hub_plugins',
    title: 'Creo hub plugin states',
    description:
      'Current state cards of all registered plugin packages: state (ACTIVE/REGISTERED/MOUNTING/UNMOUNTING/' +
      'ERROR/TAINTED/INVALID/UNSUPPORTED), autoMount, wantMount target, fingerprintChanged badge, lastErr, ' +
      'invalidReason, requires/provides, command id list. ALWAYS call this before creo_hub_mount/unmount to ' +
      'ground exact plugin ids (铁律 2: query before mutation). Read-only.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    annotations: { readOnlyHint: true, openWorldHint: false },
    handler: async () => get('/api/v1/plugins'),
  },
  {
    name: 'creo_hub_mount',
    title: 'Mount (load) a plugin into Creo',
    description:
      'Mount a registered plugin package into the LIVE Creo session (loads its DLL via Pro/TOOLKIT on the ' +
      'Creo main thread). Precondition: exact id from creo_hub_plugins; state must not be INVALID/UNSUPPORTED. ' +
      'Success = {ok:true, state:"ACTIVE"}; re-mounting an ACTIVE plugin is an idempotent success (no-op). ' +
      'Business failures return {ok:false, error, code, state, hint} - always read the hint. This is a real ' +
      'mutation of the Creo session; the hub writes an [AUDIT] line for every attempt (visible via creo_hub_log).',
    inputSchema: {
      type: 'object',
      properties: {
        id: { type: 'string', minLength: 1, maxLength: 63, description: 'exact plugin id from creo_hub_plugins' },
      },
      required: ['id'],
      additionalProperties: false,
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    handler: async (args) => {
      requireId(args);
      return post('mount', { id: args.id });
    },
  },
  {
    name: 'creo_hub_unmount',
    title: 'Unmount (unload) a plugin from Creo',
    description:
      'Unmount a plugin from the LIVE Creo session (unloads its DLL). Success = {ok:true, ' +
      'state:"REGISTERED"|"ERROR"}. If the hub answers state:"TAINTED", the DLL could not be fully released ' +
      '(PTK limitation): it may still reside in the Creo session - tell the user to restart Creo for a clean ' +
      'state and do NOT retry repeatedly. Precondition: exact id from creo_hub_plugins. Audited by the hub.',
    inputSchema: {
      type: 'object',
      properties: {
        id: { type: 'string', minLength: 1, maxLength: 63, description: 'exact plugin id from creo_hub_plugins' },
      },
      required: ['id'],
      additionalProperties: false,
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    handler: async (args) => {
      requireId(args);
      return post('unmount', { id: args.id });
    },
  },
  {
    name: 'creo_hub_call',
    title: 'Execute a plugin command (gated call)',
    description:
      'Execute one plugin command inside the live Creo session through the hub call gate (audited op=call). ' +
      'ALWAYS read creo_hub_manifest first - it is the single source of truth for exact command ids, risk, and ' +
      'inputSchema (args are schema-validated; violations return ok:false with details[] JSON pointers). ' +
      'safe commands execute immediately: {ok:true, result, ms, state}. Business failures return ok:false with ' +
      'the plugin-authored err_code/err_msg (data, not transport errors). confirm/destructive commands are GATED: ' +
      'you get {ok:false, awaitingConfirm:true, argsDigest, expiresMs} - a human must approve the exact call in ' +
      'the Creo hub panel within ~60s; tell the user to approve it there, then RETRY the same call with IDENTICAL ' +
      'args (the hub auto-pairs your retry to the approval by argsDigest; no nonce ever reaches you). ' +
      'dryRun:true previews validation + wouldExecute + confirmation requirement WITHOUT executing. ' +
      'Timeouts: report-don\'t-kill (504-style ok:false, hub stays orphaned until the late result lands).',
    inputSchema: {
      type: 'object',
      properties: {
        plugin: { type: 'string', description: 'exact plugin id (from creo_hub_plugins)' },
        command: { type: 'string', description: 'exact command id "<plugin_id>.<name>" (from creo_hub_manifest)' },
        args: { type: 'object', description: 'command arguments per its inputSchema' },
        dryRun: { type: 'boolean', description: 'true = validate + wouldExecute preview only; never executes' },
      },
      required: ['plugin', 'command'],
      additionalProperties: false,
    },
    annotations: { readOnlyHint: false, destructiveHint: true, idempotentHint: false, openWorldHint: false },
    handler: async (args) => {
      if (!args || typeof args.plugin !== 'string' || !args.plugin.trim() ||
          typeof args.command !== 'string' || !args.command.trim()) {
        throw new BridgeError('BAD_ARGS',
          'plugin and command are required (exact ids: creo_hub_plugins / creo_hub_manifest)');
      }
      if (args.args !== undefined &&
          (typeof args.args !== 'object' || args.args === null || Array.isArray(args.args))) {
        throw new BridgeError('BAD_ARGS', 'args must be a plain object per the command inputSchema');
      }
      const plugin = args.plugin.trim();
      const command = args.command.trim();
      const { status, json } = await postCall(plugin, command, args.args, { dryRun: args.dryRun === true });
      /* M3c c3: 428 -> confirmation chain (L2 elicitation when the host
       * supports it and risk allows; L3 panel projection otherwise) */
      if (status === 428) return confirmFlow(server, json, plugin, command, args.args, mapCallResult);
      return mapCallResult(status, json, plugin, command);
    },
  },
  {
    name: 'creo_hub_job',
    title: 'Async job status (poll)',
    description:
      'Query an async long-task job (a command declared async:true answers 202 + jobId instead of a result). ' +
      'Pass {jobId} for one job (state/pct/msg/elapsedMs/result), or omit it to list live/recent jobs. ' +
      'States: queued / working / completed / failed / cancelled. ' +
      'Poll until completed/failed before deciding anything; NEVER blindly retry a mutating async command - ' +
      'check the job (and creo_hub_log) first. Jobs are kept 600s after completion. Read-only.',
    inputSchema: {
      type: 'object',
      properties: {
        jobId: { type: 'string', description: 'job id from creo_hub_call 202 result (e.g. "j-3")' },
      },
      additionalProperties: false,
    },
    annotations: { readOnlyHint: true, openWorldHint: false },
    handler: async (args) => {
      const id = args && typeof args.jobId === 'string' ? args.jobId.trim() : '';
      if (id && !/^j-[0-9]+$/.test(id)) {
        throw new BridgeError('BAD_ARGS', 'jobId must look like "j-<number>" (from the 202 result of creo_hub_call)');
      }
      const r = await getStatus(id ? `/api/v1/jobs/${id}` : '/api/v1/jobs?all=1');
      if (r.status === 200) return r.json;
      if (r.status === 404) {
        return {
          ok: false,
          error: 'job not found',
          jobId: id,
          hint: 'jobs are kept 600s after completion; call creo_hub_job WITHOUT jobId to list live/recent jobs',
        };
      }
      throw new BridgeError('CONTRACT_MISMATCH',
        `unexpected status ${r.status} for GET /api/v1/jobs`,
        'hub may be an older build without the jobs face (M4b) - check creo_hub_status features');
    },
  },
  {
    name: 'creo_hub_log',
    title: 'Creo hub log (incremental tail)',
    description:
      'Incremental tail of the hub log (registry events, [AUDIT] lines for every mount/unmount/API call, ' +
      'errors). Pass offset (bytes) from the previous call to get only new lines; omit for the tail from 0 ' +
      '(capped by the hub). Returns {lines[], offset, size} - feed the returned offset into the next call. ' +
      'If offset > size the log rotated and the hub restarts you at 0. Use it to verify what an operation ' +
      'actually did ([AUDIT] op=... result=... ms=...). Read-only.',
    inputSchema: {
      type: 'object',
      properties: {
        offset: { type: 'integer', minimum: 0, description: 'byte offset from a previous creo_hub_log call' },
      },
      additionalProperties: false,
    },
    annotations: { readOnlyHint: true, openWorldHint: false },
    handler: async (args) => {
      const route =
        args && Number.isInteger(args.offset) && args.offset >= 0
          ? `/api/v1/log?offset=${args.offset}`
          : '/api/v1/log';
      return get(route);
    },
  },
];

function requireId(args) {
  if (!args || typeof args.id !== 'string' || !args.id.trim()) {
    throw new BridgeError('BAD_ARGS', 'id is required (non-empty string) - get valid ids from creo_hub_plugins');
  }
}

/* M3c c1 (docs\23 §4.3/§4.4): /call HTTP status -> model-facing shape.
 * ALLOWLIST discipline: every field is copied explicitly. The 428 body's
 * needConfirm.nonce / transactionId NEVER appear here - nonce hygiene is
 * structural, not a filter (the raw values stay inside the hub/bridge/
 * panel loop; approval is physically bound to the Creo process).
 * Business states are isError:false data with ok:false payloads (M2.5
 * docs\20 §3 semantics); only BridgeError transport failures are isError. */
function mapCallResult(status, json, plugin, command) {
  const j = (json && typeof json === 'object') ? json : {};
  if (status === 202) {
    /* M4b async: accepted, not finished. The main thread keeps running the
     * command; the model must poll creo_hub_job instead of retrying. */
    const out = { ok: true, async: true, plugin, command };
    if (j.result && typeof j.result === 'object' && j.result.jobId) out.jobId = j.result.jobId;
    if (j.poll && typeof j.poll === 'object') out.poll = j.poll;
    if (j.meta && typeof j.meta === 'object') out.meta = j.meta;
    if (j.seq !== undefined) out.seq = j.seq;
    out.hint = 'long task ACCEPTED (not finished): poll creo_hub_job {jobId} until state is ' +
      'completed/failed. The Creo main thread is busy with it (other PTK calls queue/timeout); ' +
      'do NOT blindly retry a mutating command - check the job result first';
    return out;
  }
  if (status === 200) {
    if (j.ok === true) {
      /* dryRun previews may PRE-ISSUE a confirmation ticket (启示 10); the
       * raw nonce/transactionId stay bridge-side - model sees a masked
       * projection (same shape the panel /pending feed exposes). */
      if (j.dryRun === true && j.needConfirm && typeof j.needConfirm === 'object') {
        const nc = j.needConfirm;
        const out = { ...j };
        out.needConfirm = {
          risk: nc.risk,
          command: nc.command,
          argsDigest: nc.argsDigest,
          expiresMs: nc.expiresMs,
        };
        if (nc.confirmVia) out.needConfirm.confirmVia = nc.confirmVia;
        out.hint = 'dry-run only - nothing was executed; needConfirm (masked) shows that a human ' +
          'approval in the Creo hub panel will be required for the real call with these exact args';
        return out;
      }
      return j;                                    /* executed result */
    }
    /* 200 business-failure passthrough (call_exec §6.1): the plugin-shaped
     * err_code/err_msg/hint stay authoritative; add transport facts only. */
    const out = { ...j, ok: false, plugin, command };
    if (!out.hint) {
      out.hint = 'business failure reported by the plugin (not an infrastructure error); ' +
        'fix the input or check the Creo session state';
    }
    return out;
  }
  if (status === 428) {
    /* single source: confirm.mjs projectPanelWait (the call handlers route
     * 428 through confirmFlow BEFORE reaching this mapper; the branch stays
     * as the defensive L3 projection with identical shape). */
    return projectPanelWait(j.needConfirm && typeof j.needConfirm === 'object' ? j.needConfirm : {},
      plugin, command);
  }
  if (status === 422) {
    const out = { ok: false, error: j.error || 'schema validation failed', plugin, command,
      hint: 'args violated the command inputSchema - details[] carry JSON pointers into args; ' +
        'fix them and retry (re-read creo_hub_manifest if the schema is unclear)' };
    if (Array.isArray(j.details)) out.details = j.details;
    return out;
  }
  if (status === 409) {
    const err = String(j.error || '');
    const out = { ok: false, error: err || 'conflict', plugin, command };
    if (j.state !== undefined) out.state = j.state;
    if (j.nonce_id !== undefined) out.nonce_id = j.nonce_id;   /* first-8 mask, hub-side */
    out.hint = /ARGS_MISMATCH/.test(err)
      ? 'the human-approved args differ from these args (argsDigest binding) - retry with the EXACT approved arguments'
      : /PENDING_APPROVAL/.test(err)
        ? 'approval not registered yet - wait for the human to approve in the Creo panel, then retry the same call'
        : /NONCE_EXPIRED/.test(err)
          ? 'confirmation expired (60s issued window / 10min approved grace, M3e S1b) or unknown - call again WITHOUT confirm_nonce for a fresh confirmation request'
          : 'plugin is not ACTIVE - mount it first (creo_hub_mount), then retry the call';
    return out;
  }
  if (status === 504) {
    const out = { ok: false, error: j.error || 'deadline exceeded', plugin, command,
      retryable: j.retryable === true,
      hint: "report-don't-kill: the command may still complete inside Creo. The hub stays ORPHANED " +
        '(new calls blocked) until the late result lands - watch creo_hub_log for op=call result=call.late, then retry' };
    if (j.op !== undefined) out.op = j.op;
    if (j.seq !== undefined) out.seq = j.seq;
    if (j.timeoutMs !== undefined) out.timeoutMs = j.timeoutMs;
    if (j.state !== undefined) out.state = j.state;
    return out;
  }
  if (status === 503) {
    const err = String(j.error || 'unavailable');
    const out = { ok: false, error: err, plugin, command };
    if (j.retryAfterMs !== undefined) out.retryAfterMs = j.retryAfterMs;
    out.hint = /orphan/i.test(err)
      ? 'a previous call is orphaned (deadline passed, result pending) - new calls are blocked until it resolves; watch creo_hub_log for call.late'
      : /table full/i.test(err)
        ? 'all 16 confirmation slots hold live tickets - wait for approvals to expire (<60s) or be consumed, then retry'
        : 'hub executor unavailable right now - retry shortly (creo_hub_status shows executorReady/orphaned)';
    return out;
  }
  if (status === 404) {
    return { ok: false, error: j.error || 'not found', plugin, command,
      hint: String(j.hint || 'plugin/command not in the registry - ground with creo_hub_plugins + creo_hub_manifest (exact ids)') };
  }
  if (status === 400) {
    return { ok: false, error: j.error || 'bad request', plugin, command,
      hint: 'body must be {plugin, command, args?, dryRun?} with exact ids from creo_hub_manifest' };
  }
  /* 500 and anything unforeseen: honest passthrough */
  return { ok: false, error: j.error || `hub returned status ${status}`, plugin, command,
    retryable: j.retryable === true,
    hint: String(j.hint || 'infrastructure-level failure in the hub call path; call creo_hub_log for the [AUDIT]/[CALL] lines') };
}

/* ------------------------------------------------------------ server --- */

const server = new Server(
  { name: 'creo-hub-bridge', version: '0.2.0' },
  {
    capabilities: { tools: { listChanged: true } },
    instructions:
      'creo-hub management + call plane (M3c): see, manage, and DRIVE the Creo 6 plugin registry on this ' +
      'machine. Order of use: creo_hub_status (diagnose) -> creo_hub_plugins / creo_hub_manifest (ground ids ' +
      'and command contracts) -> creo_hub_mount / creo_hub_unmount (mutate; audited) -> creo_hub_call (execute ' +
      'a plugin command; confirm/destructive commands need human approval in the Creo hub panel - the tool ' +
      'response tells you when and how to retry) -> creo_hub_log (verify). Every plugin command is ALSO ' +
      'exposed as a first-class tool named <plugin_id>__<command_short> (schema-passthrough; same gate and ' +
      'confirm semantics as creo_hub_call); the first-class face follows the live manifest - when plugins or ' +
      'descriptors change, the tool list changes with them (notifications/tools/list_changed).',
  }
);

/* ------------------------------------------- dynamic tool generations ---
 * §4.2: generation = pure projection of the current manifest; swap is
 * atomic and fail-safe (refresh failure keeps the previous generation).
 * Triggers: (a) best-effort pull before connect, (b) throttled call-along
 * health probe (immediacy), (c) 5s health poll (no-call awareness). The
 * fingerprint is model-visible via creo_hub_status/manifest payloads
 * (reconciliation face); no extra fields are injected into responses
 * (allowlist purity + token economy). */
let generation = { fingerprint: null, tools: [], degraded: [] };
let refreshInFlight = null;
let lastProbeMs = 0;

function maxToolsFromEnv() {
  const v = parseInt(process.env[MAX_TOOLS_ENV] || '', 10);
  return Number.isInteger(v) && v >= 0 ? v : DEFAULT_MAX_TOOLS;
}

async function refreshGeneration() {
  if (refreshInFlight) return refreshInFlight;
  refreshInFlight = (async () => {
    try {
      const health = await get('/api/v1/health');
      const fp = (health && typeof health.manifestFingerprint === 'string')
        ? health.manifestFingerprint : null;
      if (fp && fp === generation.fingerprint) return false;   /* unchanged: no churn */
      const manifest = await get('/api/v1/manifest');
      const gen = genTools(manifest, { maxTools: maxToolsFromEnv() });
      generation = {
        fingerprint: fp || (manifest && manifest.manifestFingerprint) || null,
        tools: gen.tools,
        degraded: gen.degraded,
      };
      console.error(`[creo-hub-bridge] tool generation swapped: fp=${generation.fingerprint} ` +
        `first-class=${gen.tools.length} degraded=${gen.degraded.map((d) => d.plugin).join(',') || '-'}`);
      try {
        await server.sendToolListChanged();
      } catch { /* pre-connect / client without support: poll + probe self-heal */ }
      return true;
    } catch (e) {
      /* fail-safe (§4.2): keep the previous generation, stay serving */
      console.error('[creo-hub-bridge] generation refresh failed (keeping previous): ' +
        String((e && e.message) || e).slice(0, 200));
      return false;
    } finally {
      refreshInFlight = null;
    }
  })();
  return refreshInFlight;
}

function probeFingerprint() {
  const now = Date.now();
  if (now - lastProbeMs < GEN_PROBE_THROTTLE_MS) return;
  lastProbeMs = now;
  refreshGeneration();   /* fire-and-forget; never delays the tool call */
}

server.setRequestHandler(ListToolsRequestSchema, async () => {
  const staticTools = TOOLS.map((t) => {
    const out = {
      name: t.name,
      title: t.title,
      description: t.description,
      inputSchema: t.inputSchema,
      annotations: t.annotations,
    };
    if (t.name === 'creo_hub_call' && generation.degraded.length > 0) {
      out.description += ' NOTE: first-class expansion is capped (' + MAX_TOOLS_ENV + '=' +
        maxToolsFromEnv() + '); these ride the generic rail only: ' +
        generation.degraded.map((d) => (d.reason === 'cap'
          ? `${d.plugin} (${d.commands} commands)`
          : `${d.plugin} (${d.reason})`)).join('; ') + '.';
    }
    return out;
  });
  const dynamicTools = generation.tools.map((t) => ({
    name: t.name,
    title: t.title,
    description: t.description,
    inputSchema: t.inputSchema,
    annotations: t.annotations,
  }));
  return { tools: [...staticTools, ...dynamicTools] };
});

async function runToolHandler(name, fn) {
  try {
    return okResult(await fn());
  } catch (e) {
    if (e instanceof BridgeError) return errorResult(e);
    /* unexpected: structured, honest, never leaking internals like tokens */
    return errorResult(
      new BridgeError('INTERNAL', 'bridge internal error while handling ' + name,
        String((e && e.message) || e).slice(0, 300)));
  }
}

server.setRequestHandler(CallToolRequestSchema, async (req) => {
  const name = req.params && req.params.name;
  const args = (req.params && req.params.arguments) || {};
  probeFingerprint();   /* §4.2 (b): every call is a generation sync point */
  const tool = TOOLS.find((t) => t.name === name);
  if (tool) return runToolHandler(name, () => tool.handler(args));
  const dyn = generation.tools.find((t) => t.name === name);
  if (dyn) {
    /* first-class rail: same gate, same confirm semantics as creo_hub_call;
     * args pass through to hub-side schema validation untouched */
    return runToolHandler(name, async () => {
      const callArgs = (args && typeof args === 'object' && !Array.isArray(args)) ? args : undefined;
      const { status, json } = await postCall(dyn.plugin, dyn.command, callArgs);
      /* first-class rail rides the SAME confirmation chain as creo_hub_call */
      if (status === 428) return confirmFlow(server, json, dyn.plugin, dyn.command, callArgs, mapCallResult);
      return mapCallResult(status, json, dyn.plugin, dyn.command);
    });
  }
  return errorResult(new BridgeError('BAD_ARGS',
    `unknown tool ${name} - ground exact ids with creo_hub_plugins / creo_hub_manifest`));
});

function okResult(obj) {
  let text = JSON.stringify(obj);
  if (text.length > TEXT_CAP) {
    /* NEVER emit invalid JSON as text (models parse it): fall back to a
     * valid digest object; hosts with structuredContent get the full data */
    const digest = { ok: obj && obj.ok, truncated: true, bytes: text.length };
    if (obj && Array.isArray(obj.lines)) {
      digest.lineCount = obj.lines.length;
      digest.offset = obj.offset;
      digest.size = obj.size;
      digest.linesTail = obj.lines.slice(-20);
    } else if (obj && Array.isArray(obj.plugins)) {
      digest.count = obj.count;
      digest.pluginIds = obj.plugins.map((p) => p && p.id);
    }
    digest.note = 'text projection truncated; full object in structuredContent';
    text = JSON.stringify(digest);
  }
  const out = { content: [{ type: 'text', text }], isError: false };
  if (obj && typeof obj === 'object' && !Array.isArray(obj)) out.structuredContent = obj;
  return out;
}

function errorResult(err) {
  const payload = err instanceof BridgeError ? err.toJSON() : { ok: false, err_code: 'INTERNAL', hint: String(err) };
  return {
    content: [{ type: 'text', text: JSON.stringify(payload) }],
    isError: true,
  };
}

/* ------------------------------------------------------------ start ---- */

/* (a) best-effort first pull BEFORE connect so a tools/list arriving right
 * after the handshake already sees the full generation (hub down = fast
 * port-refusal; pathological network gets a 2.5s budget, then we connect
 * static-only and the poll/notification path heals the client later). */
await Promise.race([
  refreshGeneration(),
  new Promise((r) => { const t = setTimeout(r, 2500); if (t.unref) t.unref(); }),
]);

const transport = new StdioServerTransport();
await server.connect(transport);

/* (c) 5s health-fingerprint poll: no-call awareness (§4.2). unref'd so it
 * never keeps the process alive past stdio close. */
const genPoll = setInterval(() => { refreshGeneration(); }, GEN_POLL_MS);
if (genPoll.unref) genPoll.unref();

console.error(`[creo-hub-bridge] connected (stdio); root=${resolveRoot()}; ` +
  `tools=${TOOLS.length}+${generation.tools.length} first-class; fp=${generation.fingerprint || 'n/a'}`);
