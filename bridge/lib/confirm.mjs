/* =====================================================================
 * creo-hub bridge - confirmation chain L2/L3 (M3c c3, docs\23 §4.3)
 *
 * The hub answers a gated call with 428 + needConfirm{transactionId,
 * nonce(32hex), risk, command, argsDigest(16hex), summary, expiresMs,
 * confirmVia?}. From there:
 *
 *   L3 (panel route - DSH reality + destructive ALWAYS):
 *     project an awaitingConfirm payload (allowlist - nonce/txn are
 *     structurally absent); the human approves in the Creo hub panel;
 *     the model retries the SAME args WITHOUT any nonce and the hub
 *     auto-pairs by (plugin,command,argsDigest). Approval is physically
 *     bound to the Creo process.
 *
 *   L2 (elicitation route - host-agnostic generic path):
 *     if the client declared elicitation.form (SDK server-side requires
 *     exactly that for form mode) AND risk is not panel-required, the
 *     BRIDGE keeps the nonce in-closure and asks the human through the
 *     host (elicitInput, flat schema {approve: enum yes/no}):
 *       accept+yes -> register the approval (/confirm, panel-button parity)
 *                     then retry /call WITH confirm_nonce (one-shot)
 *       accept+no / decline -> structured refusal (data, not error)
 *       cancel -> aborted, no retry
 *     Any elicitation failure collapses to L3 (fail-safe).
 *
 * Nonce hygiene is structural: the raw 32-hex value only ever exists in
 * this module's locals and the retry request body. Digest-8 (first 8 hex
 * of argsDigest) is the human-facing correlation token (panel parity).
 *
 * argsDigest 预检 (D-c3-1): the bridge retries with the SAME retained
 * args reference (no mutation window between 428 and retry), so the
 * digest matches by construction; the hub's NONCE_ARGS_MISMATCH remains
 * the authority (no JS re-implementation of the C canonicalization -
 * drift risk rejected).
 * ===================================================================== */
import { post, postCall } from './hub_client.mjs';

/* flat-primitive requestedSchema (MCP elicitation constraint): one enum
 * question, no nested objects. */
export const APPROVE_SCHEMA = {
  type: 'object',
  properties: {
    approve: {
      type: 'string',
      enum: ['yes', 'no'],
      description: 'approve execution of this exact call inside the live Creo session?',
    },
  },
  required: ['approve'],
};

/* SDK 1.30 server.elicitInput form mode requires clientCapabilities
 * .elicitation.form to be truthy (client/index.js getSupportedElicitation
 * Modes treats {} as form-capable for backwards compat, but the SERVER
 * check is strict - mirror it exactly so we never die inside elicitInput). */
export function supportsElicitation(server) {
  try {
    const caps = server.getClientCapabilities();
    return !!(caps && caps.elicitation && caps.elicitation.form);
  } catch {
    return false;
  }
}

export function buildElicitMessage(nc, plugin, command) {
  const digest8 = String(nc.argsDigest || '').slice(0, 8);
  const parts = [
    'Human approval required to execute a plugin command inside the LIVE Creo session.',
    `command: ${command}`,
    `plugin: ${plugin}`,
    `risk: ${nc.risk || 'confirm'}`,
  ];
  if (nc.summary) parts.push(`summary: ${nc.summary}`);
  parts.push(`args digest: ${digest8} (matches the pending ticket in the Creo hub panel)`);
  parts.push(`the request expires in ~${Math.round((nc.expiresMs || 60000) / 1000)}s; ` +
    'approving retries this exact call once (one-shot credential). Decline or cancel executes nothing.');
  return parts.join('\n');
}

/* L3 projection - the EXACT c1 shape (battery J1-J3 pins these fields).
 * Allowlist discipline: nonce / transactionId are not copied, period. */
export function projectPanelWait(nc, plugin, command, extraNote) {
  const out = {
    ok: false,
    awaitingConfirm: true,
    plugin,
    command: nc.command || command,
    risk: nc.risk,
    argsDigest: nc.argsDigest,
    expiresMs: nc.expiresMs,
  };
  if (nc.confirmVia) out.confirmVia = nc.confirmVia;
  out.hint = (nc.confirmVia === 'panel-required')
    ? 'DESTRUCTIVE: a human must approve this exact call in the Creo hub panel (PENDING ticket dialog) ' +
      'within ~60s; then RETRY the same call with IDENTICAL args - the hub auto-pairs your retry to the ' +
      'approval by argsDigest. Approval is bound to the Creo process; no nonce ever reaches the model.'
    : 'confirmation required before execution: a human must approve this exact call in the Creo hub ' +
      'panel, then retry the same call unchanged (auto-pair by argsDigest)';
  if (extraNote) out.hint += ' ' + extraNote;
  return out;
}

/* 428 entry point. Returns the model-facing payload directly:
 *   - L3 projection (panel route / collapse),
 *   - executed result (mapped through mapFn),
 *   - {ok:false, declined:true|cancelled:true, ...} structured refusal.
 * Never throws for confirm-state reasons; transport failures from the
 * accept-retry propagate as BridgeError (same as any call). */
export async function confirmFlow(server, body, plugin, command, args, mapFn) {
  const nc = (body && typeof body.needConfirm === 'object' && body.needConfirm) ? body.needConfirm : {};
  const panelForced = nc.confirmVia === 'panel-required';
  if (panelForced || !supportsElicitation(server)) {
    return projectPanelWait(nc, plugin, command);
  }
  let res;
  try {
    res = await server.elicitInput({
      message: buildElicitMessage(nc, plugin, command),
      requestedSchema: APPROVE_SCHEMA,
    });
  } catch (e) {
    /* host declared the capability but the request failed (protocol race,
     * handler error, shutdown): fail-safe collapse to the panel route. */
    return projectPanelWait(nc, plugin, command,
      '(host elicitation unavailable: ' + String((e && e.message) || e).slice(0, 120) + ')');
  }
  const action = res && res.action;
  const approve = res && res.content ? String(res.content.approve || '').toLowerCase() : '';
  if (action === 'accept' && approve === 'yes') {
    /* D-c3-2 (hub state machine, call_exec hint text is authoritative):
     * the nonce is a PAIRING credential, not the authorization - the
     * ticket must reach APPROVED first. The human just approved through
     * the host's elicitation UI, so the bridge (token holder) registers
     * that approval exactly like the panel button does, then consumes
     * the ticket with the nonce retry. Registration failure (expiry race)
     * is non-fatal: the retry then answers 409 NONCE_EXPIRED with a hint. */
    try {
      await post('confirm', { transactionId: nc.transactionId, approve: true });
    } catch { /* the nonce retry below is the authoritative step */ }
    /* nonce leaves this closure exactly once, into the retry body */
    const retry = await postCall(plugin, command, args, { confirmNonce: nc.nonce });
    return mapFn(retry.status, retry.json, plugin, command);
  }
  if (action === 'accept' /* approve === 'no' */ || action === 'decline') {
    return {
      ok: false,
      declined: true,
      plugin,
      command,
      risk: nc.risk,
      argsDigest: nc.argsDigest,
      hint: 'the human DECLINED this call - nothing was executed; do not retry unless the human ' +
        'asks for it (the pending ticket expires unused in <60s)',
    };
  }
  /* cancel (or an unrecognized action - treat as the most conservative) */
  return {
    ok: false,
    cancelled: true,
    plugin,
    command,
    risk: nc.risk,
    argsDigest: nc.argsDigest,
    hint: 'the confirmation dialog was CANCELLED/dismissed - nothing was executed and no retry was ' +
      'attempted; call again for a fresh confirmation if this call is still wanted',
  };
}
