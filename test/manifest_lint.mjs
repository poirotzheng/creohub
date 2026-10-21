/* =====================================================================
 * creo-hub M3b b4: manifest_lint - OFFLINE plugin package validator.
 * Second implementation of the registry.c parse rules (docs\23 §6.4 [2],
 * DoD D5): both faces are pinned to the SAME fixtures + golden verdicts
 * (test\lint_fixtures, test\lint_expected.txt) so lint and hub can never
 * drift apart silently.
 *
 * Usage:
 *   node test\manifest_lint.mjs <pluginsRoot>                 human report
 *   node test\manifest_lint.mjs <pluginsRoot> --golden <file> contract mode:
 *        compares verdicts against the golden file; exit = mismatch count
 * Human mode exit code: number of INVALID packages (UNSUPPORTED warns).
 *
 * Scope mirrors parse_plugin_dir_inner: JSON shape + on-disk existence.
 * Runtime-only concerns (dependency resolution, cycles, states) belong to
 * the registry/depgraph face, NOT to lint.
 * ===================================================================== */
import { readFileSync, existsSync, statSync, readdirSync } from "node:fs";
import { join } from "node:path";

const MAX_REQ = 8;
const ID_RE = /^[a-z0-9-]{1,63}$/;
const VER_RE = /^[0-9A-Za-z.+-]{1,31}$/;
const RISKS = ["safe", "confirm", "destructive"];
const CONTEXTS = ["part", "assembly", "any"];

const invalid = (reason) => ({ verdict: "INVALID", reason });
const unsupported = (reason) => ({ verdict: "UNSUPPORTED", reason });
const OK = { verdict: "OK", reason: "" };

function isObj(v) { return v !== null && typeof v === "object" && !Array.isArray(v); }

/* one manifest object against the shared rule set; `base` = dir the
 * entry/textDir paths resolve against; `expectId` = pointer id binding */
function lintManifest(m, base, expectId) {
  if (!isObj(m)) return invalid("plugin.json parse error (syntax)");
  const mv = m.manifestVersion;
  if (mv !== 1 && mv !== 2) return invalid("manifestVersion missing or not in {1,2}");
  const id = m.id;
  if (typeof id !== "string" || !ID_RE.test(id))
    return invalid("id missing/invalid (need 1-63 chars of [a-z0-9-])");
  if (expectId !== null && id !== expectId)
    return invalid(`version manifest id '${id}' != pointer id '${expectId}'`);
  if (typeof m.adapter !== "string" || !m.adapter) return invalid("adapter missing");
  if (m.adapter !== "ptk-sync")
    return unsupported(`adapter '${m.adapter}' not supported in M1 (ptk-sync only)`);
  if (typeof m.entry !== "string" || !m.entry) return invalid("entry missing");
  if (!existsSync(join(base, m.entry)) || !statSync(join(base, m.entry)).isFile())
    return invalid(`entry file not found: ${m.entry}`);
  const td = (typeof m.textDir === "string" && m.textDir) ? m.textDir : "text";
  const ta = join(base, td, "text", "usascii");
  if (!existsSync(ta) || !statSync(ta).isDirectory())
    return invalid("textDir lacks text\\usascii subdir (Creo appends \\text!)");
  if (m.hubCmdExport !== undefined &&
      (typeof m.hubCmdExport !== "string" || !m.hubCmdExport || m.hubCmdExport.length >= 64))
    return invalid("hubCmdExport must be a non-empty string (< 64 chars)");
  if (m.callRoute !== undefined && m.callRoute !== "A" && m.callRoute !== "B")
    return invalid('callRoute must be "B" (direct, default) or "A" (task)');
  if (m.commands !== undefined) {
    if (!Array.isArray(m.commands))
      return invalid(mv === 2 ? "v2: commands must be an array" : "commands must be an array");
    for (let n = 0; n < m.commands.length; n++) {
      const c = m.commands[n];
      if (!isObj(c)) return invalid(`v2: commands[${n}] must be an object`);
      if (typeof c.id !== "string" || !c.id)
        return invalid(`v2: commands[${n}].id missing`);
      if (!c.id.startsWith(id + "."))
        return invalid(`command id '${c.id}' must start with '${id}.'`);
      if (mv === 2) {
        for (const f of ["title", "desc", "descForLLM"])
          if (typeof c[f] !== "string")
            return invalid(`v2: commands[${n}].${f} missing or not a string (required)`);
        if (typeof c.risk !== "string")
          return invalid(`v2: commands[${n}].risk missing or not a string (required: safe|confirm|destructive)`);
        if (!RISKS.includes(c.risk))
          return invalid(`v2: commands[${n}].risk invalid (must be safe|confirm|destructive)`);
        if (!isObj(c.inputSchema))
          return invalid(`v2: commands[${n}].inputSchema missing or not an object (required)`);
        if (c.inputSchema.type !== undefined && c.inputSchema.type !== "object")
          return invalid(`v2: commands[${n}].inputSchema.type must be "object"`);
        if (c.inputSchema.properties !== undefined && !isObj(c.inputSchema.properties))
          return invalid(`v2: commands[${n}].inputSchema.properties must be an object`);
        if (c.timeoutSec !== undefined && (typeof c.timeoutSec !== "number" || !(c.timeoutSec > 0)))
          return invalid(`v2: commands[${n}].timeoutSec must be a number > 0`);
        if (c.context !== undefined && !CONTEXTS.includes(c.context))
          return invalid(`v2: commands[${n}].context must be part|assembly|any`);
        for (const b of ["reversible", "idempotent"])
          if (c[b] !== undefined && typeof c[b] !== "boolean")
            return invalid(`v2: commands[${n}].${b} must be a boolean`);
        /* M4b additive fields (docs\39 3.1/3.5/3.6) - same rules as registry.c */
        if (c.async !== undefined && typeof c.async !== "boolean")
          return invalid(`v2: commands[${n}].async must be a boolean`);
        if (c.undoCommand !== undefined) {
          if (typeof c.undoCommand !== "string" || !c.undoCommand.startsWith(id + "."))
            return invalid(`v2: commands[${n}].undoCommand must be '<id>.<cmd>' in the same package`);
          if (c.undoCommand === c.id)
            return invalid(`v2: commands[${n}].undoCommand must not reference itself`);
        }
        if (c.group !== undefined && (typeof c.group !== "string" || !c.group || c.group.length > 32))
          return invalid(`v2: commands[${n}].group must be a non-empty string (max 32)`);
      }
    }
    if (mv === 2) {
      const ids = new Set();
      for (const c of m.commands) if (isObj(c) && typeof c.id === "string") ids.add(c.id);
      for (let n = 0; n < m.commands.length; n++) {
        const c = m.commands[n];
        if (isObj(c) && typeof c.undoCommand === "string" && !ids.has(c.undoCommand))
          return invalid(`v2: commands[${n}].undoCommand '${c.undoCommand}' not found in this package`);
      }
    }
  }
  for (const f of ["requires", "provides"]) {
    if (m[f] !== undefined) {
      if (!Array.isArray(m[f])) return invalid(`v2: ${f} must be an array of strings`);
      for (let n = 0; n < m[f].length; n++)
        if (typeof m[f][n] !== "string") return invalid(`v2: ${f}[${n}] must be a string`);
      if (m[f].length > MAX_REQ) return invalid(`v2: ${f} has ${m[f].length} entries (max 8 in M2)`);
    }
  }
  return OK;
}

function readJson(path) {
  let raw;
  try { raw = readFileSync(path, "utf8"); } catch (e) { return { err: invalid("plugin.json not readable") }; }
  try { return { json: JSON.parse(raw) }; } catch (e) { return { err: invalid("plugin.json parse error (syntax)") }; }
}

function lintPackage(dir) {
  const rootPath = join(dir, "plugin.json");
  const r = readJson(rootPath);
  if (r.err) return r.err;
  const root = r.json;
  if (!isObj(root)) return invalid("plugin.json parse error (syntax)");
  if (root.manifestVersion !== 1 && root.manifestVersion !== 2)
    return invalid("manifestVersion missing or not in {1,2}");
  if (typeof root.id !== "string" || !ID_RE.test(root.id))
    return invalid("id missing/invalid (need 1-63 chars of [a-z0-9-])");
  if (typeof root.adapter !== "string" || !root.adapter) return invalid("adapter missing");
  if (root.adapter !== "ptk-sync")
    return unsupported(`adapter '${root.adapter}' not supported in M1 (ptk-sync only)`);
  /* b3 P1 thin pointer redirection (one level; version charset gate = b4) */
  if (typeof root.currentVersion === "string" && root.currentVersion) {
    const cv = root.currentVersion;
    if (!VER_RE.test(cv))
      return invalid(`currentVersion '${cv}': invalid version chars (need 1-31 of [0-9A-Za-z.+-])`);
    const vpath = join(dir, "versions", cv, "plugin.json");
    if (!existsSync(vpath))
      return invalid(`currentVersion '${cv}': versions\\${cv}\\plugin.json missing`);
    const vr = readJson(vpath);
    if (vr.err) return vr.err;
    return lintManifest(vr.json, join(dir, "versions", cv), root.id);
  }
  return lintManifest(root, dir, null);
}

/* ------------------------------------------------------------------ main */
const argv = process.argv.slice(2);
if (argv.length < 1) {
  console.log("usage: manifest_lint.mjs <pluginsRoot> [--golden <file>]");
  process.exit(2);
}
const pluginsRoot = argv[0];
const goldenIdx = argv.indexOf("--golden");
const goldenFile = goldenIdx >= 0 ? argv[goldenIdx + 1] : null;

const dirs = readdirSync(pluginsRoot, { withFileTypes: true })
  .filter((d) => d.isDirectory())
  .map((d) => d.name)
  .sort();

const results = new Map();
for (const name of dirs) {
  if (!existsSync(join(pluginsRoot, name, "plugin.json"))) continue; /* not a package */
  results.set(name, lintPackage(join(pluginsRoot, name)));
}

let bad = 0;
if (!goldenFile) {
  for (const [name, r] of results) {
    if (r.verdict === "OK") console.log(`OK          ${name}`);
    else { console.log(`${r.verdict.padEnd(12)}${name}: ${r.reason}`); if (r.verdict === "INVALID") bad++; }
  }
  console.log(`\n=== LINT RESULT: ${results.size - bad} usable, ${bad} INVALID ===`);
  process.exit(bad);
}

/* golden mode: the D5 contract face. Both sides (this lint, registry.c via
 * v2_manifest_test) must reproduce the golden verdicts over the SAME tree. */
const golden = readFileSync(goldenFile, "utf8").split(/\r?\n/)
  .filter((l) => l.trim() && !l.startsWith("#"))
  .map((l) => l.split(/\s+/));
let mismatch = 0;
for (const [dir, verdict, kw] of golden) {
  const r = results.get(dir);
  if (!r) { console.log(`[FAIL] ${dir}: not linted (missing from tree)`); mismatch++; continue; }
  if (r.verdict !== verdict) {
    console.log(`[FAIL] ${dir}: golden ${verdict} != lint ${r.verdict} (${r.reason})`);
    mismatch++;
  } else if (verdict !== "OK" && kw && kw !== "-" && !r.reason.includes(kw)) {
    console.log(`[FAIL] ${dir}: reason '${r.reason}' lacks golden keyword '${kw}'`);
    mismatch++;
  } else {
    console.log(`[PASS] ${dir} ${verdict}`);
  }
}
/* extra packages in the tree that the golden does not know = drift too */
for (const name of results.keys())
  if (!golden.some((g) => g[0] === name)) { console.log(`[FAIL] ${name}: not in golden file`); mismatch++; }
console.log(`\n=== LINT-GOLDEN RESULT: ${golden.length} pairs, ${mismatch} mismatch -> ${mismatch ? "FAIL" : "ALL PASS"} ===`);
process.exit(mismatch ? 1 : 0);
