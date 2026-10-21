/* =====================================================================
 * creo-hub bridge - first-class tool generation (M3c c2, docs\23 §4.1)
 *
 * Pure projection: manifest JSON -> MCP tool definitions. No I/O, no
 * state - the tool generation is a pure function of the current manifest
 * (confluence: bridge restart = re-pull = same generation, §4.2).
 *
 * Naming contract (§4.1):
 *   rawName  = <plugin_id>__<cmd_short>
 *   cmd_short = command id minus "<plugin_id>." prefix, sanitized
 *               ([^A-Za-z0-9_] -> "_")
 *   DSH full name = mcp__creohub__<rawName>  (host-side, not ours)
 *   rawName > 64 chars (MCP protocol limit) -> truncate + append
 *   "_" + FNV-1a/32 of the FULL command id, first 4 hex (uniqueness by
 *   registry composite key transitivity + DSH sanitize-collision hash as
 *   double insurance - M2.5 evidence).
 *
 * Schema contract: manifest inputSchema passes through VERBATIM (the M2
 * whitelist subset is valid JSON Schema is valid MCP inputSchema - zero
 * transformation). A command whose schema is missing/not object-rooted
 * does NOT expand (defensive; registration should have caught it) and
 * rides the generic rail only.
 *
 * Cap (§4.1 降级规则): at most opts.maxTools (env CREO_HUB_MAX_TOOLS,
 * default 32) first-class tools; a plugin whose whole command set does
 * not fit degrades ENTIRELY to the generic rail (token economy, 启示 6 -
 * the cap is a guardrail, not the normal state).
 * ===================================================================== */

export const MCP_NAME_MAX = 64;
export const DEFAULT_MAX_TOOLS = 32;

/* FNV-1a 32-bit - same family the hub uses for fingerprints (16-hex is
 * the 64-bit variant there; 4 hex chars here are a collision hint, the
 * registry composite key remains the authority). */
export function fnv1a32(str) {
  let h = 0x811c9dc5;
  const s = String(str);
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i) & 0xff;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

export const fnvHex4 = (s) => fnv1a32(s).toString(16).padStart(8, '0').slice(0, 4);

export function sanitizeShort(s) {
  return String(s).replace(/[^A-Za-z0-9_]/g, '_');
}

/* plugin ids keep MCP-legal chars verbatim ('-' is legal in tool names and
 * real in fixture/field ids like p-ok); only protocol-illegal chars fold. */
function sanitizePid(s) {
  return String(s).replace(/[^A-Za-z0-9_-]/g, '_');
}

export function makeRawName(pluginId, commandId) {
  const pid = sanitizePid(pluginId);
  let short = String(commandId).startsWith(pluginId + '.')
    ? String(commandId).slice(pluginId.length + 1)
    : String(commandId);
  let raw = `${pid}__${sanitizeShort(short)}`;
  if (raw.length > MCP_NAME_MAX) {
    const suffix = '_' + fnvHex4(commandId);
    raw = raw.slice(0, MCP_NAME_MAX - suffix.length) + suffix;
  }
  return raw;
}

/* risk/idempotent -> MCP annotations (§4.1 mapping; C JSON emits numeric
 * bools (0/1) - accept both forms. reversible/timeoutSec have no standard
 * hint: they stay in description/manifest). */
export function annotationsFor(cmd) {
  const risk = String((cmd && cmd.risk) || 'safe').toLowerCase();
  const ann = { openWorldHint: false };
  if (risk === 'safe') {
    ann.readOnlyHint = true;
  } else {
    ann.readOnlyHint = false;
    if (risk === 'destructive' || risk === 'confirm') ann.destructiveHint = true;
  }
  if (cmd && (cmd.idempotent === true || cmd.idempotent === 1)) ann.idempotentHint = true;
  return ann;
}

function validCmd(c) {
  return c && typeof c.id === 'string' && c.id.length > 0;
}

function objectRootedSchema(c) {
  return c && c.inputSchema && typeof c.inputSchema === 'object' &&
    !Array.isArray(c.inputSchema) && c.inputSchema.type === 'object';
}

/* manifest (the /api/v1/manifest payload) -> { tools, degraded }.
 * tools[] entries carry {name,title,description,inputSchema,annotations,
 * plugin,command}; the bridge strips plugin/command before listing. */
export function genTools(manifest, opts = {}) {
  const cap = Number.isInteger(opts.maxTools) && opts.maxTools >= 0
    ? opts.maxTools : DEFAULT_MAX_TOOLS;
  const tools = [];
  const degraded = [];
  const seen = new Set();
  /* /api/v1/manifest emits the package array under "plugins" (web_api.c
   * get_manifest) - each entry carries the FULL v2 command contracts. */
  const packages = (manifest && Array.isArray(manifest.plugins)) ? manifest.plugins : [];
  for (const pkg of packages) {
    if (!pkg || typeof pkg.id !== 'string' || !pkg.id) continue;
    /* only v2 manifests carry command contracts (risk/descForLLM/
     * inputSchema); v1 packages ride the generic rail - a known class,
     * not an anomaly, so no degraded entry. */
    if (pkg.manifestVersion !== undefined && pkg.manifestVersion !== 2) continue;
    const cmds = (Array.isArray(pkg.commands) ? pkg.commands : []).filter(validCmd);
    if (cmds.length === 0) continue;                    /* nothing to expand (INVALID) */
    if (tools.length + cmds.length > cap) {             /* whole-plugin degrade (§4.1) */
      degraded.push({ plugin: pkg.id, commands: cmds.length, reason: 'cap' });
      continue;
    }
    for (const cmd of cmds) {
      if (!objectRootedSchema(cmd)) {                   /* per-command defensive degrade */
        degraded.push({ plugin: pkg.id, commands: 1, reason: 'schema:' + cmd.id });
        continue;
      }
      let name = makeRawName(pkg.id, cmd.id);
      if (seen.has(name)) {                             /* pathological collision: re-suffix */
        name = (name.slice(0, MCP_NAME_MAX - 5) + '_' + fnvHex4(cmd.id + '#' + pkg.id));
      }
      seen.add(name);
      tools.push({
        name,
        title: cmd.id,
        description: (typeof cmd.descForLLM === 'string' && cmd.descForLLM.length > 0)
          ? cmd.descForLLM
          : `Execute ${cmd.id} (plugin ${pkg.id}) through the creo-hub gate.`,
        inputSchema: cmd.inputSchema,                   /* VERBATIM passthrough */
        annotations: annotationsFor(cmd),
        plugin: pkg.id,
        command: cmd.id,
      });
    }
  }
  return { tools, degraded };
}
