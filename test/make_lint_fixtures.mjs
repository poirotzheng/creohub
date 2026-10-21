/* =====================================================================
 * creo-hub M3b b4: manifest_lint fixture generator (D5 contract test).
 * Regenerates test\lint_fixtures\* and test\lint_expected.txt (the golden
 * verdict file). The golden is DATA; manifest_lint.mjs (node) and
 * registry.c (C, via v2_manifest_test D5 phase) are the TWO independent
 * implementations checked against it - that pairing is the anti-drift
 * contract (docs\23 §6.4, DoD D5).
 *   node test\make_lint_fixtures.mjs
 * ===================================================================== */
import { mkdirSync, writeFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(fileURLToPath(new URL(".", import.meta.url)), "lint_fixtures");
const goldenPath = join(fileURLToPath(new URL(".", import.meta.url)), "lint_expected.txt");

rmSync(root, { recursive: true, force: true });
mkdirSync(root, { recursive: true });

function files(dir, { entry = "bin/pkg.dll", text = true } = {}) {
  if (entry) {
    mkdirSync(join(root, dir, "bin"), { recursive: true });
    writeFileSync(join(root, dir, entry), "MZ-fake-entry");
  }
  if (text) {
    mkdirSync(join(root, dir, "text", "text", "usascii"), { recursive: true });
    writeFileSync(join(root, dir, "text", "text", "usascii", "pkg_messages.txt"), "#MSG fixture\n");
  }
}

function pkg(dir, manifest, opts) {
  mkdirSync(join(root, dir), { recursive: true });
  writeFileSync(join(root, dir, "plugin.json"), manifest);
  files(dir, opts);
}

function ver(dir, ver, manifest) {
  const base = join(root, dir, "versions", ver);
  mkdirSync(join(base, "bin"), { recursive: true });
  mkdirSync(join(base, "text", "text", "usascii"), { recursive: true });
  writeFileSync(join(base, "bin", "pkg.dll"), "MZ-fake-entry");
  writeFileSync(join(base, "text", "text", "usascii", "pkg_messages.txt"), "#MSG fixture\n");
  writeFileSync(join(base, "plugin.json"), manifest);
}

const cmd = (id, extra) => JSON.stringify(Object.assign({
  id, title: "T " + id, desc: "d", descForLLM: "llm", risk: "safe",
  inputSchema: { type: "object", properties: {} },
}, extra || {}));

/* ---- verdicts: [dir, verdict, reasonKeyword] (golden, in scan order) ---- */
const golden = [];
const G = (dir, verdict, kw) => golden.push(dir + " " + verdict + " " + (kw || "-"));

/* --- good: classic v1 (zero-migration layout) --- */
pkg("good-classic", JSON.stringify({
  manifestVersion: 1, id: "good-classic", adapter: "ptk-sync",
  entry: "bin/pkg.dll", textDir: "text", version: "1.0.0",
}));
G("good-classic", "OK");

/* --- good: full v2 (every optional field exercised) --- */
pkg("good-v2-full", JSON.stringify({
  manifestVersion: 2, id: "good-v2-full", name: "Good V2 Full", version: "2.1.0",
  adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  hubCmdExport: "hub_cmd", callRoute: "B",
  requires: ["good-classic"], provides: ["gv2.svc"],
  commands: [
    JSON.parse(cmd("good-v2-full.work", { context: "part", timeoutSec: 5,
      reversible: true, idempotent: false })),
    JSON.parse(cmd("good-v2-full.wipe", { risk: "destructive", context: "any" })),
  ],
}));
G("good-v2-full", "OK");

/* --- good: b3 P1 pointer package (versions 1.0.0 / 1.1.0) --- */
mkdirSync(join(root, "good-pointer"), { recursive: true });
writeFileSync(join(root, "good-pointer", "plugin.json"), JSON.stringify({
  manifestVersion: 2, id: "good-pointer", adapter: "ptk-sync", currentVersion: "1.0.0",
}));
ver("good-pointer", "1.0.0", JSON.stringify({
  manifestVersion: 2, id: "good-pointer", name: "Good Pointer", version: "1.0.0",
  adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("good-pointer.one"))],
}));
ver("good-pointer", "1.1.0", JSON.stringify({
  manifestVersion: 2, id: "good-pointer", name: "Good Pointer", version: "1.1.0",
  adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("good-pointer.eleven"))],
}));
G("good-pointer", "OK");

/* --- bad: JSON syntax --- */
mkdirSync(join(root, "bad-syntax"), { recursive: true });
writeFileSync(join(root, "bad-syntax", "plugin.json"), '{"manifestVersion":2, broken');
G("bad-syntax", "INVALID", "syntax");

/* --- bad: id missing --- */
pkg("bad-noid", JSON.stringify({
  manifestVersion: 1, adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
}));
G("bad-noid", "INVALID", "id");

/* --- bad: manifestVersion out of enum --- */
pkg("bad-mv", JSON.stringify({
  manifestVersion: 3, id: "bad-mv", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
}));
G("bad-mv", "INVALID", "manifestVersion");

/* --- unsupported adapter (UNSUPPORTED class, NOT invalid) --- */
pkg("bad-adapter", JSON.stringify({
  manifestVersion: 1, id: "bad-adapter", adapter: "ptk-async",
  entry: "bin/pkg.dll", textDir: "text",
}));
G("bad-adapter", "UNSUPPORTED", "adapter");

/* --- bad: risk enum violation --- */
pkg("bad-risk", JSON.stringify({
  manifestVersion: 2, id: "bad-risk", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-risk.go", { risk: "danger" }))],
}));
G("bad-risk", "INVALID", "risk");

/* --- bad: inputSchema not an object --- */
pkg("bad-schema", JSON.stringify({
  manifestVersion: 2, id: "bad-schema", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [{ id: "bad-schema.go", title: "t", desc: "d", descForLLM: "l",
               risk: "safe", inputSchema: "none" }],
}));
G("bad-schema", "INVALID", "inputSchema");

/* --- bad: command id namespace violation --- */
pkg("bad-ns", JSON.stringify({
  manifestVersion: 2, id: "bad-ns", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("wrong.prefix"))],
}));
G("bad-ns", "INVALID", "must start with");

/* --- bad: entry file missing on disk --- */
pkg("bad-entry", JSON.stringify({
  manifestVersion: 1, id: "bad-entry", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
}), { entry: false });
G("bad-entry", "INVALID", "entry");

/* --- bad: textDir lacks text\usascii (Creo appends \text!) --- */
pkg("bad-textdir", JSON.stringify({
  manifestVersion: 1, id: "bad-textdir", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
}), { text: false });
G("bad-textdir", "INVALID", "textDir");

/* --- bad: timeoutSec must be > 0 --- */
pkg("bad-timeout", JSON.stringify({
  manifestVersion: 2, id: "bad-timeout", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-timeout.go", { timeoutSec: 0 }))],
}));
G("bad-timeout", "INVALID", "timeoutSec");

/* --- bad: context enum violation --- */
pkg("bad-context", JSON.stringify({
  manifestVersion: 2, id: "bad-context", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-context.go", { context: "space" }))],
}));
G("bad-context", "INVALID", "context");

/* --- bad: requires over the MAX_REQ=8 cap --- */
pkg("bad-requires", JSON.stringify({
  manifestVersion: 2, id: "bad-requires", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  requires: ["r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9"],
}));
G("bad-requires", "INVALID", "requires");

/* --- bad: pointer id != version manifest id --- */
mkdirSync(join(root, "bad-mismatch"), { recursive: true });
writeFileSync(join(root, "bad-mismatch", "plugin.json"), JSON.stringify({
  manifestVersion: 2, id: "bad-mismatch", adapter: "ptk-sync", currentVersion: "1.0.0",
}));
ver("bad-mismatch", "1.0.0", JSON.stringify({
  manifestVersion: 2, id: "other-id", adapter: "ptk-sync", version: "1.0.0",
  entry: "bin/pkg.dll", textDir: "text",
}));
G("bad-mismatch", "INVALID", "pointer id");

/* --- bad: currentVersion points at a missing version dir --- */
mkdirSync(join(root, "bad-ptrver"), { recursive: true });
writeFileSync(join(root, "bad-ptrver", "plugin.json"), JSON.stringify({
  manifestVersion: 2, id: "bad-ptrver", adapter: "ptk-sync", currentVersion: "9.9.9",
}));
G("bad-ptrver", "INVALID", "plugin.json missing");

/* --- bad: currentVersion charset gate (b4 hardening: no path escapes) --- */
mkdirSync(join(root, "bad-verchars"), { recursive: true });
writeFileSync(join(root, "bad-verchars", "plugin.json"), JSON.stringify({
  manifestVersion: 2, id: "bad-verchars", adapter: "ptk-sync", currentVersion: "../evil",
}));
G("bad-verchars", "INVALID", "invalid version chars");

/* --- M4b: additive contract fields (async / undoCommand / group) --- */
pkg("good-v2-m4", JSON.stringify({
  manifestVersion: 2, id: "good-v2-m4", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [
    JSON.parse(cmd("good-v2-m4.batch", { async: true, timeoutSec: 60, group: "batch" })),
    JSON.parse(cmd("good-v2-m4.undo", { risk: "confirm", undoCommand: "good-v2-m4.batch", group: "batch" })),
  ],
}));
G("good-v2-m4", "OK");

pkg("bad-async", JSON.stringify({
  manifestVersion: 2, id: "bad-async", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-async.go", { async: "yes" }))],
}));
G("bad-async", "INVALID", "async");

pkg("bad-undo-self", JSON.stringify({
  manifestVersion: 2, id: "bad-undo-self", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-undo-self.a", { undoCommand: "bad-undo-self.a" }))],
}));
G("bad-undo-self", "INVALID", "must not reference itself");

pkg("bad-undo-missing", JSON.stringify({
  manifestVersion: 2, id: "bad-undo-missing", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-undo-missing.a", { undoCommand: "bad-undo-missing.nope" }))],
}));
G("bad-undo-missing", "INVALID", "not found");

pkg("bad-group", JSON.stringify({
  manifestVersion: 2, id: "bad-group", adapter: "ptk-sync", entry: "bin/pkg.dll", textDir: "text",
  commands: [JSON.parse(cmd("bad-group.go", { group: "" }))],
}));
G("bad-group", "INVALID", "group");

writeFileSync(goldenPath,
  "# creo-hub D5 golden verdicts: <dir> <OK|UNSUPPORTED|INVALID> <reasonKeyword|->\n" +
  "# shared by manifest_lint.mjs (node) and registry.c (C, v2_manifest_test D5 phase)\n" +
  golden.join("\n") + "\n");

console.log("fixtures: " + golden.length + " packages -> " + root);
console.log("golden:   " + goldenPath);
