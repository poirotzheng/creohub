#!/usr/bin/env node
/**
 * creo-hub ask gate - PreToolUse command hook (deploy\hooks.json).
 *
 * Purpose (Q3 route A, docs\21 ISSUE-B2):
 *   DSH 0.1.5-rc.1 has NO per-tool permission-rule config surface and does
 *   NOT read MCP annotations (readOnlyHint/destructiveHint). Approval popups
 *   fire only when a `tools/pre-execute` listener returns {kind:"ask"}.
 *   The shipped @deepseek-ai/dsh-hooks-claude-code bridge maps a PreToolUse
 *   hook's `hookSpecificOutput.permissionDecision:"ask"` onto exactly that
 *   (dsh-hook-protocol parseHookOutput -> mergeHookOutputs -> serviceAsk ->
 *   dsh-user-approval). This script is the hook command.
 *
 * Contract (verified against dsh-hook-protocol lib, field by field):
 *   - stdin : PreToolUse payload JSON {tool_name, tool_input, tool_use_id, ...}
 *   - stdout: ONE JSON object, exit code 0:
 *       {"hookSpecificOutput":{"hookEventName":"PreToolUse",
 *         "permissionDecision":"ask","permissionDecisionReason":"..."}}
 *     hookEventName MUST equal "PreToolUse" or event-scoped fields are
 *     discarded by the expectedEventName guard.
 *   - The hooks.json matcher (M3c c3, Q-M3c-2 ratified negative-lookahead
 *     regex) restricts this hook to every mcp__creohub__* tool EXCEPT the
 *     four read-only ones (status/plugins/manifest/log), so the decision is
 *     the constant "ask"; stdin is consumed only to enrich the reason with
 *     the actual tool name (and to avoid EPIPE on the writer side).
 *   - Fails soft: unreadable/unparsable stdin still prints a valid ask JSON;
 *     never exits non-zero. hooks.json sets timeout:10 as the hang backstop
 *     (a timed-out/failed hook degrades to NO decision = allow, so hanging
 *     is the only real failure mode - hence the explicit timeout).
 */
import { readFileSync } from 'node:fs';

const FALLBACK_TOOL = 'mcp__creohub__* (gated mutation/call)';

let tool = FALLBACK_TOOL;
try {
  const payload = JSON.parse(readFileSync(0, 'utf8'));
  if (payload && typeof payload.tool_name === 'string' && payload.tool_name.length > 0) {
    tool = payload.tool_name;
  }
} catch {
  /* stdin absent or unparsable: keep the generic tool label */
}

const out = {
  hookSpecificOutput: {
    hookEventName: 'PreToolUse',
    permissionDecision: 'ask',
    permissionDecisionReason:
      `creo-hub mutation gate: ${tool} acts on the LIVE Creo session ` +
      '(loads/unloads a plugin DLL or executes a plugin command). Approve only if this call was intended.',
  },
};
process.stdout.write(JSON.stringify(out) + '\n');
