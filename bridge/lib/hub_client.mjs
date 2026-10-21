/* =====================================================================
 * creo-hub bridge - hub HTTP client (docs\20 §4)
 *
 * Stateless by design (confluence invariant, docs\19 §1): nothing
 * about the hub is cached across tool calls. Every call re-discovers the
 * port and RE-READS <root>\hub_token.txt (G4 rotation semantics: the hub
 * rewrites the token on every start, so caching it would break restart
 * recovery).
 *
 * Security invariants (docs\20 §1):
 *   - zero listening sockets (this module only makes outbound loopback
 *     requests);
 *   - the token never appears in thrown error details or returned values
 *     (only its presence/validity status does);
 *   - env vars are NEVER used for the token (DSH scrubs TOKEN-like names
 *     from stdio child env - file route is the only viable one, and the
 *     only designed one).
 * ===================================================================== */
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const PORT_MIN = 8765;             /* M2 scan band, same as hub + panel   */
const PORT_MAX = 8775;
const PROBE_TIMEOUT_MS = 800;      /* per-port discovery probe           */
const GET_TIMEOUT_MS = 8000;       /* read-only requests                 */
const POST_TIMEOUT_MS = 15000;     /* > hub-side G1 TIMEOUT ms=10000     */
const CALL_POST_TIMEOUT_MS = 35000;/* > worst /call envelope: 30s budget cap
                                    * + 500ms (call_exec clamp [1,30000]) */
const BUSY_RETRY_MAX = 3;          /* panel-parity cap (approval fatigue)*/

/* M3c c1 (docs\23 §4.4, 启示 7): version-pin handshake. The bridge refuses
 * to operate against a hub that does not declare the exact contract it was
 * built for - an M4 multi-version coexistence must never fail silently.
 * Pin = what the bridge tool face actually consumes: manifest (tool_gen
 * source) + call (execution rails). panel/versioning stay panel-side. */
const PINNED_API_VERSION = 'v1';
const PINNED_FEATURES = ['manifest', 'call'];

/**
 * Pure version-pin check (exported for battery unit asserts).
 * Throws BridgeError('VERSION_MISMATCH') unless health declares
 * apiVersion v1 + every pinned feature.
 */
export function checkVersionPin(health) {
  const av = health && health.apiVersion;
  const feats = (health && Array.isArray(health.features)) ? health.features : [];
  if (av !== PINNED_API_VERSION) {
    throw new BridgeError(
      'VERSION_MISMATCH',
      `hub apiVersion is ${JSON.stringify(av)} but this bridge is pinned to ` +
      `"${PINNED_API_VERSION}" - rebuild/restart Creo with the matching creo_hub.dll ` +
      '(or use the bridge release that matches the running hub)',
      `apiVersion=${av}`);
  }
  const missing = PINNED_FEATURES.filter((f) => feats.indexOf(f) < 0);
  if (missing.length) {
    throw new BridgeError(
      'VERSION_MISMATCH',
      `hub is missing required feature(s): ${missing.join(', ')} - this bridge needs a ` +
      'creo-hub M3b+ build (features: panel/manifest/call/versioning); ' +
      'rebuild the DLL and restart Creo',
      `features=${feats.join('|')}`);
  }
}

const __dirname = path.dirname(fileURLToPath(import.meta.url));

/** repo root = bridge\lib -> ..\.. ; env CREO_HUB_ROOT overrides. */
export function resolveRoot() {
  const env = process.env.CREO_HUB_ROOT;
  if (env && env.trim()) return path.resolve(env.trim());
  return path.resolve(__dirname, '..', '..');
}

/** Structured bridge error (铁律 5): always {err_code, hint, detail?}. */
export class BridgeError extends Error {
  constructor(err_code, hint, detail) {
    super(`${err_code}: ${hint}`);
    this.err_code = err_code;
    this.hint = hint;
    if (detail !== undefined) this.detail = detail;
  }
  toJSON() {
    const o = { ok: false, err_code: this.err_code, hint: this.hint };
    if (this.detail !== undefined) o.detail = this.detail;
    return o;
  }
}

/* ---------------- token (fresh read, never cached, never echoed) ------- */

async function readToken(root) {
  const file = path.join(root, 'hub_token.txt');
  let raw;
  try {
    raw = await readFile(file, 'utf8');
  } catch (e) {
    throw new BridgeError(
      'TOKEN_MISSING',
      `cannot read ${file} - is this the same root the hub runs with? ` +
      '(hub writes the token next to its plugins dir on every start)',
      `code=${e.code}`);
  }
  const tok = raw.trim();
  if (!/^[0-9a-fA-F]{16,128}$/.test(tok)) {
    /* do NOT include the content in the error (security invariant) */
    throw new BridgeError(
      'TOKEN_MISSING',
      `${file} does not look like a hub token (expected hex string); ` +
      'refusing to use it',
      `length=${tok.length}`);
  }
  return tok;
}

/* ---------------- low-level request ------------------------------------ */

async function requestJson(port, method, route, token, bodyObj, timeoutMs) {
  const url = `http://127.0.0.1:${port}${route}`;
  const headers = { 'X-Hub-Token': token };
  let body;
  if (bodyObj !== undefined) {
    headers['Content-Type'] = 'application/json';
    body = JSON.stringify(bodyObj);
  }
  let res;
  try {
    res = await fetch(url, { method, headers, body, signal: AbortSignal.timeout(timeoutMs) });
  } catch (e) {
    const timedOut = e && e.name === 'TimeoutError';
    throw new BridgeError(
      'HTTP_TRANSPORT',
      timedOut
        ? `request to ${method} ${route} timed out after ${timeoutMs}ms`
        : `cannot reach hub at 127.0.0.1:${port}`,
      timedOut ? undefined : String(e && e.cause ? e.cause.code || e.cause.message : e.message));
  }
  let json = null;
  const text = await res.text().catch(() => '');
  try { json = text ? JSON.parse(text) : null; } catch { /* non-JSON: contract drift */ }
  if (json === null || typeof json !== 'object') {
    throw new BridgeError(
      'CONTRACT_MISMATCH',
      `hub returned non-JSON for ${method} ${route} (status ${res.status}) - ` +
      'hub too old, or this port is not creo_hub (another program?)',
      `status=${res.status} bytes=${text.length}`);
  }
  return { status: res.status, json };
}

/* ---------------- discovery -------------------------------------------- */

async function probe(port, token) {
  try {
    const { status, json } = await requestJson(port, 'GET', '/api/v1/health', token, undefined, PROBE_TIMEOUT_MS);
    if (status === 200 && json.ok === true &&
        Array.isArray(json.features) && json.features.indexOf('panel') >= 0) {
      return { port, health: json };
    }
    if (status === 401) return { port, unauthorized: true };
    return null;
  } catch {
    return null;
  }
}

/**
 * Find the live hub: scan the M2 port band in parallel, require
 * ok:true + features contains "panel" (G5 contract) with a VALID token.
 * Returns {port, health, token, root}. Throws BridgeError with precise
 * self-healing hints otherwise.
 * opts.pin === false skips the version-pin check (creo_hub_status only:
 * the diagnostic tool must be able to REPORT a mismatched hub, not die
 * on it - every other route pins).
 */
export async function discover(opts = {}) {
  const root = resolveRoot();
  const token = await readToken(root);
  const ports = [];
  for (let p = PORT_MIN; p <= PORT_MAX; p++) ports.push(p);
  const results = await Promise.all(ports.map((p) => probe(p, token)));

  const live = results.find((r) => r && r.port !== undefined && r.health);
  if (live) {
    if (opts.pin !== false) checkVersionPin(live.health);
    return { port: live.port, health: live.health, token, root };
  }

  /* every port answered but rejected the token -> rotated/foreign root */
  const sawUnauthorized = results.some((r) => r && r.unauthorized);
  if (sawUnauthorized) {
    throw new BridgeError(
      'TOKEN_STALE',
      'a hub answered but rejected the token from hub_token.txt - the hub ' +
      'restarted with a different root, or the file is from another install; ' +
      'verify CREO_HUB_ROOT points at the running hub root');
  }
  throw new BridgeError(
    'HUB_UNREACHABLE',
    'no creo_hub listening on 127.0.0.1:8765..8775 - start it in Creo: ' +
    'Tools menu -> Auxiliary Applications -> creo_hub -> Start ' +
    '(Creo itself must be running on THIS machine; the hub binds loopback only)');
}

/* ---------------- op execution ----------------------------------------- */

/**
 * Run one GET op with discovery. `fn(ctx)` receives {port, token} and
 * returns the parsed hub response object.
 */
export async function get(route, opts = {}) {
  const { port, token } = await discover();
  const { status, json } = await requestJson(port, 'GET', route, token, undefined, GET_TIMEOUT_MS);
  if (status === 401) {
    /* rotation race: hub restarted between read and call - one fresh retry */
    const fresh = await readToken(resolveRoot());
    const r2 = await requestJson(port, 'GET', route, fresh, undefined, GET_TIMEOUT_MS);
    if (r2.status === 401) throw new BridgeError('TOKEN_STALE', 'hub rejected a freshly-read token (root mismatch?)');
    return r2.json;
  }
  if (status !== 200) {
    throw new BridgeError('CONTRACT_MISMATCH', `unexpected status ${status} for GET ${route}`,
      json && json.error ? String(json.error) : undefined);
  }
  if (opts.expectOk !== false && json.ok !== true) {
    throw new BridgeError('CONTRACT_MISMATCH', `GET ${route} answered ok:false`, json.error);
  }
  return json;
}

/**
 * Run one POST op (mount/unmount) with discovery + busy retry.
 * Business failures (200 + ok:false) are RETURNED, not thrown - they are
 * data for the model (docs\20 §3 failure semantics), enriched with hint.
 */
export async function post(op, bodyObj) {
  const { port, token } = await discover();
  const route = `/api/v1/${op}`;
  let attempt = 0;
  for (;;) {
    let status, json;
    try {
      ({ status, json } = await requestJson(port, 'POST', route, token, bodyObj, POST_TIMEOUT_MS));
    } catch (e) {
      if (e instanceof BridgeError && e.err_code === 'HTTP_TRANSPORT' && attempt < BUSY_RETRY_MAX) {
        /* transport blip mid-op: bounded retry, same cap as busy */
        attempt++;
        await sleep(500 * attempt);
        continue;
      }
      throw e;
    }
    if (status === 401) {
      const fresh = await readToken(resolveRoot());
      const r2 = await requestJson(port, 'POST', route, fresh, bodyObj, POST_TIMEOUT_MS);
      if (r2.status === 401) throw new BridgeError('TOKEN_STALE', 'hub rejected a freshly-read token (root mismatch?)');
      ({ status, json } = r2);
    }
    if (status === 503) {
      const err = json && json.error;
      if (err === 'busy') {
        if (attempt >= BUSY_RETRY_MAX) {
          throw new BridgeError('BUSY_TIMEOUT',
            `hub queue stayed busy after ${BUSY_RETRY_MAX} retries - a long operation ` +
            'is occupying the Creo main thread; call creo_hub_log to see what is running');
        }
        attempt++;
        await sleep(Number(json.retryAfterMs) > 0 ? Number(json.retryAfterMs) : 500);
        continue;
      }
      if (err === 'hub not ready') {
        throw new BridgeError('HUB_NOT_READY',
          'hub is up but its executor is not ready yet (Creo still initializing) - retry in a few seconds');
      }
      if (err === 'hub stopping') {
        throw new BridgeError('HUB_STOPPING',
          'hub is shutting down (Creo closing or auxiliary-app Stop) - restart it before retrying');
      }
      throw new BridgeError('CONTRACT_MISMATCH', `unexpected 503 for POST ${route}`, err);
    }
    if (status !== 200) {
      throw new BridgeError('CONTRACT_MISMATCH', `unexpected status ${status} for POST ${route}`,
        json && json.error ? String(json.error) : undefined);
    }
    if (json.ok === false) return enrichBusinessFailure(op, json);
    return json;
  }
}

/**
 * M3c c1 (docs\23 §4.4): POST /api/v1/call with discovery + busy retry.
 * Unlike post(), EVERY HTTP status is RESOLVED as {status, json} - the
 * call contract's 4xx/5xx are meaningful business states (422 schema /
 * 428 needConfirm / 409 nonce / 504 deadline) that the tool handler maps
 * to model-facing shapes by ALLOWLIST (nonce hygiene: the 428 body carries
 * the raw nonce, which must never reach the model - only the handler's
 * allowlisted projection does). Transport-level failures still throw
 * BridgeError (HUB_UNREACHABLE / TOKEN_* / BUSY_TIMEOUT / HTTP_TRANSPORT).
 */
export async function postCall(plugin, command, args, opts = {}) {
  const body = { plugin, command };
  if (args !== undefined && args !== null) body.args = args;
  if (opts.dryRun === true) body.dryRun = true;
  /* M3c c3: the L2 elicitation accept path resubmits the one-shot execution
   * credential. confirmNonce is BRIDGE-SIDE ONLY (confirm.mjs closure) - it
   * is a body field here and never appears in any returned/mapped payload. */
  if (typeof opts.confirmNonce === 'string' && /^[0-9a-f]{32}$/.test(opts.confirmNonce)) {
    body.confirm_nonce = opts.confirmNonce;
  }
  const { port, token } = await discover();
  let attempt = 0;
  for (;;) {
    let status, json;
    try {
      ({ status, json } = await requestJson(port, 'POST', '/api/v1/call', token, body, CALL_POST_TIMEOUT_MS));
    } catch (e) {
      if (e instanceof BridgeError && e.err_code === 'HTTP_TRANSPORT' && attempt < BUSY_RETRY_MAX) {
        attempt++;
        await sleep(500 * attempt);
        continue;
      }
      throw e;
    }
    if (status === 401) {
      /* rotation race: one fresh-read retry, same as post() */
      const fresh = await readToken(resolveRoot());
      const r2 = await requestJson(port, 'POST', '/api/v1/call', fresh, body, CALL_POST_TIMEOUT_MS);
      if (r2.status === 401) throw new BridgeError('TOKEN_STALE', 'hub rejected a freshly-read token (root mismatch?)');
      ({ status, json } = r2);
    }
    if (status === 503 && json && json.error === 'busy') {
      if (attempt >= BUSY_RETRY_MAX) {
        throw new BridgeError('BUSY_TIMEOUT',
          `hub queue stayed busy after ${BUSY_RETRY_MAX} retries - a long operation ` +
          'is occupying the Creo main thread; call creo_hub_log to see what is running');
      }
      attempt++;
      await sleep(Number(json.retryAfterMs) > 0 ? Number(json.retryAfterMs) : 500);
      continue;
    }
    return { status, json };
  }
}

/** GET that RESOLVES every HTTP status (like postCall): for endpoints whose
 * non-200 answers are DATA, e.g. GET /api/v1/jobs/<id> -> 404 job not found
 * must reach the model as a structured not-found, not a transport error. */
export async function getStatus(route) {
  const { port, token } = await discover();
  const { status, json } = await requestJson(port, 'GET', route, token, undefined, GET_TIMEOUT_MS);
  return { status, json };
}

/** 200+ok:false business failure -> same object + model-facing hint. */
function enrichBusinessFailure(op, json) {
  const out = { ...json };
  const err = String(json.error || '');
  if (err === 'plugin not found') {
    out.hint = 'id not in registry - call creo_hub_plugins first and use an exact id (grounding before mutation, 铁律 2)';
  } else if (/failed$/.test(err)) {
    const code = String(json.code || '');
    const hints = {
      /* op-sensitive: on mount the registry REFUSES with E_IN_USE but the
       * API answers ok:true (idempotent), so an ok:false E_IN_USE here is
       * the UNMOUNT path = Creo refused to release the DLL. */
      E_IN_USE: op === 'mount'
        ? 'plugin is already mounted (state ACTIVE) - no action needed'
        : 'Creo refused to release the DLL (still in use by the session)',
      CANT_ACCESS: 'entry DLL missing or unreadable - check the package bin\\ path exists',
      INVALID_FILE: 'entry is not a valid PTK DLL (wrong exports/arch) - see docs\\18 §5',
      APP_UNLOCK: 'PTK license not available in this Creo session',
      MSG_NOT_FOUND: 'message file missing under <textDir>\\text\\usascii\\ - see docs\\18 §9',
      E_NOT_FOUND: 'package vanished from disk - call creo_hub_status then rescan via panel',
    };
    out.hint = hints[code] ||
      `Pro/TOOLKIT error ${code || 'unknown'} during ${op}; state=${json.state}. ` +
      'call creo_hub_log for the [AUDIT] line and surrounding context';
    if (json.state === 'TAINTED') {
      out.hint += ' | TAINTED: unload failed, DLL may still reside in the Creo session - ' +
        'restart Creo for a clean state; do NOT retry unmount repeatedly';
    }
  } else if (/id required/i.test(err)) {
    out.hint = 'pass {"id":"<plugin id>"} - get valid ids from creo_hub_plugins';
  }
  return out;
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }
