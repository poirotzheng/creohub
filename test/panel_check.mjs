/* =====================================================================
 * creo-hub M2c panel verification (node, no npm deps):
 *   node panel_check.mjs static  <indexHtml>            -- ES5 purity + zero external refs (O7+ static half)
 *   node panel_check.mjs dump    <dumpFile> <token>     -- headless-Chrome rendered-DOM assertions
 *   node panel_check.mjs contract <port> <token>        -- live API contract the panel codes against
 * Exit code = failure count.
 * ===================================================================== */
import { readFileSync } from "node:fs";
import http from "node:http";

const mode = process.argv[2];
let pass = 0, fail = 0;
const ok = (cond, name) => {
  if (cond) { pass++; console.log("[PASS] " + name); }
  else { fail++; console.log("[FAIL] " + name); }
};

/* ------------------------------------------------------------ static */
if (mode === "static") {
  const file = process.argv[3];
  const html = readFileSync(file, "utf8");
  /* ALL inline <script> blocks are scanned (bootstrap diagnostic block +
   * main panel block); external src= is separately banned below. */
  const blocks = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((b) => b[1]);
  ok(blocks.length >= 1, "inline <script> blocks found: " + blocks.length);
  /* strip comments before the purity scan: comments may legitimately NAME
   * banned constructs (e.g. "no fetch/Promise") without using them.
   * (no string in this panel contains // or /* so stripping is safe) */
  const strip = (s) => s
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/(^|[^:"'])\/\/[^\n]*/g, "$1");
  const js = blocks.map(strip).join("\n");

  /* ES5 purity (IE11 baseline): no ES6+ syntax anywhere in the script */
  const banned = [
    [/=>/, "arrow function"],
    [/\blet\s+[A-Za-z_$]/, "let binding"],
    [/\bconst\s+[A-Za-z_$]/, "const binding"],
    [/`/, "template literal"],
    [/\bfetch\s*\(/, "fetch()"],
    [/\bPromise\b/, "Promise"],
    [/\bclass\s+[A-Za-z_$]/, "class"],
    [/\.\.\./, "spread/rest"],
    [/\basync\b|\bawait\b/, "async/await"],
    [/\bSymbol\b|\bMap\s*\(|\bSet\s*\(/, "ES6 builtins"],
    [/\.includes\s*\(/, "Array/String.includes (ES7)"],
    [/\bObject\.assign\b/, "Object.assign (ES6)"],
  ];
  for (const [re, name] of banned) ok(!re.test(js), "ES5 purity: no " + name);

  /* zero external references anywhere (O7+ air-gap static half) */
  ok(!/https?:\/\//.test(html), "O7+ no absolute http(s) URL anywhere in file");
  ok(!/<script[^>]+src\s*=/i.test(html), "O7+ no external <script src>");
  ok(!/<link[^>]+href\s*=/i.test(html), "O7+ no <link> (stylesheets/fonts)");
  ok(!/@import/i.test(html), "O7+ no CSS @import");
  ok(!/url\s*\(/i.test(html), "O7+ no CSS url() resources");
  ok(!/<img/i.test(html), "O7+ no <img>");
  ok(/<meta[^>]+charset/i.test(html), "meta charset present (UTF-8 Chinese)");
  ok(/X-UA-Compatible/i.test(html), "X-UA-Compatible IE=edge meta present");

  /* security-critical behaviors present in code */
  ok(/X-Hub-Token/.test(js), "L3 token header wired");
  ok(/replaceState/.test(js), "L3 URL token scrub (replaceState)");
  ok(/function esc\(/.test(js), "HTML escaper defined");
  /* three-way self-diagnostics (2026-09-13 field finding: silent "connecting"
   * gave no evidence; noscript/onerror/watchdog make the failure mode visible) */
  ok(/<noscript>/.test(html), "self-diagnostic: noscript guidance present");
  ok(/window\.onerror/.test(js), "self-diagnostic: onerror bar installed");
  ok(/__hubAlive/.test(js), "self-diagnostic: watchdog wired to health success");
  const escCount = (js.match(/esc\(/g) || []).length;
  ok(escCount >= 25, "escaper used broadly (" + escCount + " call sites)");
  ok(!/document\.write/.test(js), "no document.write");
  ok(/innerHTML/.test(js) && !/outerHTML/.test(js), "innerHTML only (with esc) - no outerHTML tricks");
  /* R13 (field P5 bug): log pane must NOT append via innerHTML += nor trim
   * by string-cutting - the DOM/node-based path is the only safe one. */
  ok(!/\.innerHTML\s*\+=/.test(js), "no innerHTML += append anywhere (P5 trim bug class)");
  ok(/createTextNode/.test(js) && /removeChild/.test(js), "log pane uses DOM nodes + node-wise trim");

  /* spec colors (09 §2.4) */
  for (const c of ["#0d1117", "#161b22", "#30363d", "#e6edf3", "#58a6ff", "#3fb950", "#d29922", "#f85149"])
    ok(html.includes(c), "spec color " + c);
  /* 7 badge states mapped */
  for (const s of ["运行中", "未挂载", "过渡", "错误(可重试)", "污染(需重启Creo)", "无效", "不支持"])
    ok(html.includes(s), "badge text " + s);

  /* b4 面板四件套 + R4 (docs\25 b4): presence in code */
  ok(/\/pending/.test(js), "b4-1 confirm feed: /pending polled");
  ok(/cfmOverlay/.test(html) && /cfmList/.test(html), "b4-1 confirm dialog DOM present");
  ok(/confirmTxn/.test(js), "b4-1 approve/deny handlers wired");
  ok(/substring\(0,\s*8\)/.test(js), "b4-1 argsDigest/txn truncated to first 8 (masking discipline)");
  ok(/倒计时/.test(js) && /countdownTick/.test(js), "b4-1 60s countdown (server remainingMs + 1s local tick)");
  ok(/id="cfmMinBtn"/.test(html) && /toggleCfmMin/.test(js),
     "M3e Q2 drawer minimize/expand toggle wired (P-1)");
  ok(/#cfmOverlay\s*\{[^}]*pointer-events:\s*none/.test(html),
     "M3e Q2 overlay non-modal: panel operable while drawer open (P-1)");
  ok(/cfmSlimRow/.test(html) && /cfmSlimRow/.test(js),
     "M3e Q2 approved tickets collapse to slim status rows (P-2)");
  ok(/id="opmodebar"/.test(html) && /applyOpmode/.test(js) && /renderOpmode/.test(js),
     "M3e Q1 operator mode bar + apply/render wired (M0/M1/M2)");
  ok(/omAck/.test(html) && /ack/.test(js),
     "M3e Q1 M2 arming demands explicit ack checkbox");
  ok(/availableVersions/.test(js) && /verIdx/.test(js), "b4-2 version column + switch/rollback handlers");
  ok(/verPick/.test(js) && /verPickIdx/.test(js) &&
     /onchange="Hub\.verPickIdx\(/.test(js),
     "b4-2fix dropdown selection survives 2s re-render (verPick, ISSUE-b4-1)");
  ok(/verPick\[p\.id\] === p\.currentVersion/.test(js),
     "b4-2fix verPick auto-clears when server currentVersion catches up");
  ok(/cardsKey/.test(js) && /lastCardsKey/.test(js) &&
     /if \(key === lastCardsKey\) return/.test(js),
     "b4-2fix steady-state render skip keeps dropdown popup open (ISSUE-b4-2)");
  ok(/drainedList/.test(js) && /将级联卸载/.test(js), "b4-3 reconcile plan shown before cascade unmount");
  ok(/dryRun/.test(js), "b4-3 dryRun plan fetched (never executes)");
  ok(/orphaned/.test(js) && /孤儿命令执行中/.test(js), "b4-4 orphan red light in topbar");
  ok(/canMount/.test(js) && /canUnmount/.test(js), "R4 button enable follows the state machine");
  ok(/flash-ok/.test(js) && /flash-err/.test(js) && /lastOkAt/.test(js),
     "R4 row color driven by op result (freshness window)");
  ok(!/Hub\.op\('unmount'/.test(html) || /unmountIdx/.test(js), "R4 unmount goes through plan-then-confirm path");
  console.log(`\n=== STATIC RESULT: ${pass} pass, ${fail} fail -> ${fail ? "FAIL" : "ALL PASS"} ===`);
  process.exit(fail ? 1 : 0);
}

/* ------------------------------------------------------------ dump */
if (mode === "dump") {
  const file = process.argv[3];
  const token = process.argv[4] || "";
  const d = readFileSync(file, "utf8");
  ok(d.includes("creo-hub"), "panel title rendered");
  ok(d.includes("p-ok") && d.includes("Demo OK"), "plugin p-ok card rendered (name from manifest)");
  ok(d.includes("未挂载"), "REGISTERED badge text rendered");
  ok(d.includes("无效") && d.includes("无效原因"), "INVALID badge + reason rendered");
  ok(d.includes("p-fail"), "p-fail card rendered");
  ok(d.includes("命令 2"), "G6 command count button rendered (p-ok has 2 commands)");
  ok(d.includes("适配器"), "adapter meta rendered");
  ok(/ptk-sync/.test(d), "adapter value rendered");
  ok(/[0-9a-f]{16}/.test(d), "R2 fingerprint hex rendered");
  ok(d.includes("日志"), "log console rendered");
  ok(d.includes("助手（预留）"), "G6 chat placeholder tab rendered");
  ok(d.includes(" Mount") || d.includes(">Mount<"), "Mount button rendered");
  ok(d.includes("autoMount"), "autoMount toggle rendered");
  ok(d.includes("共 <b") || /共/.test(d), "stats rendered");
  /* seeded log lines fetched through /api/v1/log and appended */
  ok(d.includes("scanned 3 package") || d.includes("[AUDIT]"), "log lines pulled into console pane");
  /* LOG_CAP trim path (R13, field P5 bug): seed = 495 fillers + 5 scenario
   * lines; cap keeps the NEWEST 400 => oldest marker trimmed away, newest
   * marker + [AUDIT] line must survive. Old innerHTML-based trim wiped the
   * whole box; these asserts fail loudly if that bug class returns. */
  ok(d.includes("LOGTRIM-MARKER-494"), "trim: newest filler line survived");
  ok(!d.includes("LOGTRIM-MARKER-000)"), "trim: oldest filler line trimmed away (cap works)");
  ok(d.includes("LOGTRIM-MARKER-100") || d.includes("LOGTRIM-MARKER-105"),
     "trim: line near cap boundary survived");
  ok(d.includes("[AUDIT] op=mount plugin=p-ok"), "trim: seeded AUDIT line (newest tier) survived");
  /* token must NOT appear anywhere in the rendered DOM */
  ok(token === "" || !d.includes(token), "token never rendered into DOM");
  /* diagnostics must NOT have fired: no rendered jserr bar (script SOURCE
   * mentions "jserr" as a string, so match the rendered div exactly) */
  ok(!d.includes('<div id="jserr"'), "no diagnostic bar fired (no JS error, no watchdog)");
  ok(!d.includes("连接中…") || d.includes("运行"), "health arrived (stats replaced connecting text)");

  /* ---- b4 四件套 rendered evidence (api_serve seeds p-ver + 1 live ticket) ---- */
  ok(/id="cfmOverlay"[^>]*class="show"/.test(d) || /class="show"[^>]*id="cfmOverlay"/.test(d),
     "b4-1 confirm dialog VISIBLE (seeded destructive ticket rendered)");
  ok(d.includes("r-destr") && d.includes("destructive"), "b4-1 red destructive badge rendered");
  ok(d.includes("Purge cache"), "b4-1 command summary (title) rendered");
  ok(d.includes("args 摘要") && /[0-9a-f]{8}…/.test(d), "b4-1 argsDigest first-8 rendered (masked)");
  ok(d.includes("批准") && d.includes("拒绝"), "b4-1 approve/deny buttons rendered");
  ok(/\d+s 倒计时/.test(d), "b4-1 countdown rendered with seconds");
  ok(!/[0-9a-f]{32}/.test(d.replace(new RegExp(token, "g"), "")),
     "b4-1 full 32-hex nonce never reaches the DOM (token scrubbed first)");
  ok(d.includes("p-ver"), "b4-2 pointer package card rendered");
  ok(d.includes("当前版本") && /id="ver-p-ver"/.test(d), "b4-2 version column + dropdown rendered");
  ok(d.includes("1.0.0") && d.includes("1.1.0"), "b4-2 both versions offered in dropdown");
  ok(d.includes("切换") && d.includes("回滚"), "b4-2 switch/rollback buttons rendered");
  ok(/id="unm-p-ok"[^>]*disabled/.test(d), "R4 unmount disabled while REGISTERED (state-machine interlock)");
  ok(!/id="mnt-p-ok"[^>]*disabled/.test(d), "R4 mount enabled while REGISTERED");
  console.log(`\n=== DUMP RESULT: ${pass} pass, ${fail} fail -> ${fail ? "FAIL" : "ALL PASS"} ===`);
  process.exit(fail ? 1 : 0);
}

/* ------------------------------------------------------------ contract */
if (mode === "contract") {
  const port = parseInt(process.argv[3], 10);
  const token = process.argv[4];

  const req = (method, path, body, hdrToken) => new Promise((res, rej) => {
    const data = body == null ? null : JSON.stringify(body);
    const r = http.request({
      host: "127.0.0.1", port, path, method,
      headers: Object.assign({
        "Host": "127.0.0.1:" + port,
        "Connection": "close",
      }, hdrToken !== false ? { "X-Hub-Token": hdrToken || token } : {},
         data != null ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) } : {}),
      timeout: 8000,
    }, (resp) => {
      let buf = "";
      resp.on("data", (c) => (buf += c));
      resp.on("end", () => {
        let j = null;
        try { j = JSON.parse(buf); } catch (e) {}
        res({ status: resp.statusCode, j, raw: buf, headers: resp.headers });
      });
    });
    r.on("error", rej);
    r.on("timeout", () => { r.destroy(new Error("timeout")); });
    r.end(data);
  });

  /* health: exact G5 shape the panel renders */
  let r = await req("GET", "/api/v1/health");
  ok(r.status === 200 && r.j && r.j.ok === true, "health 200 ok");
  ok(r.j.apiVersion === "v1", "health apiVersion v1");
  ok(Array.isArray(r.j.features) && r.j.features.includes("panel") && r.j.features.includes("manifest"),
     "health features [panel,manifest]");
  ok(typeof r.j.port === "number" && r.j.port === port, "health port matches");
  ok(typeof r.j.uptimeSec === "number", "health uptimeSec number");
  ok(r.j.executorReady === true, "health executorReady true");
  ok(typeof r.j.queueUsed === "number", "health queueUsed number");
  /* b4: global manifest generation (docs\23 4.2 source c - bridge 5s poll) */
  ok(/^[0-9a-f]{16}$/.test(r.j.manifestFingerprint || ""), "b4 health manifestFingerprint 16-hex");
  const rh2 = await req("GET", "/api/v1/health");
  ok(rh2.j.manifestFingerprint === r.j.manifestFingerprint,
     "b4 manifestFingerprint stable across polls (pure function of manifests)");

  /* plugins: every field the panel card renderer reads */
  r = await req("GET", "/api/v1/plugins");
  ok(r.status === 200 && r.j.ok === true && r.j.count === 4, "plugins 200 count=4 (p-ok/p-fail/p-bad/p-ver)");
  const pok = r.j.plugins.find((p) => p.id === "p-ok");
  const pbad = r.j.plugins.find((p) => p.id === "p-bad");
  ok(!!pok, "p-ok present");
  for (const f of ["id", "name", "version", "adapter", "state", "manifestVersion",
                   "autoMount", "wantMount", "fingerprint", "fingerprintChanged",
                   "commands", "requires", "provides"])
    ok(pok && f in pok, "p-ok has field " + f);
  ok(pok && pok.name === "Demo OK" && pok.version === "1.2.0", "p-ok name/version from manifest v2");
  ok(pok && /^[0-9a-f]{16}$/.test(pok.fingerprint), "p-ok fingerprint 16-hex");
  ok(pok && pok.wantMount === -1, "p-ok wantMount=-1 (unset)");
  ok(pok && pok.requires.includes("core.base") && pok.provides.includes("ok.svc"),
     "requires/provides arrays populated");
  ok(pok && pok.commands.length === 2, "p-ok commands id list (2)");
  ok(pbad && pbad.state === "INVALID" && typeof pbad.invalidReason === "string" && pbad.invalidReason.length > 0,
     "p-bad INVALID + invalidReason");
  ok(!r.raw.includes("handle") && !r.raw.includes("plugins\\"), "no handle / host paths in plugins JSON");

  /* manifest: command contract fields for G6 view */
  r = await req("GET", "/api/v1/manifest");
  ok(r.status === 200 && r.j.ok === true, "manifest 200 ok");
  const mok = r.j.plugins.find((p) => p.id === "p-ok");
  ok(mok && mok.commands.length === 2, "manifest p-ok has 2 full commands");
  const c0 = mok && mok.commands[0], c1 = mok && mok.commands[1];
  for (const f of ["id", "title", "desc", "descForLLM", "risk", "reversible", "idempotent", "timeoutSec", "inputSchema"])
    ok(c0 && f in c0, "command[0] has field " + f);
  ok(c0 && c0.risk === "safe" && c1 && c1.risk === "destructive", "risk enum passthrough (safe/destructive)");
  ok(c1 && c1.timeoutSec === 30 && c1.reversible === 0 && c1.idempotent === 1, "typed optionals passthrough");
  ok(c1 && c1.inputSchema && c1.inputSchema.type === "object" && "dry" in c1.inputSchema.properties,
     "inputSchema embedded object");
  ok(c0 && typeof c0.descForLLM === "string" && c0.descForLLM.length > 0, "descForLLM non-empty (bridge/agent face)");

  /* log endpoint incremental contract */
  r = await req("GET", "/api/v1/log?offset=0");
  ok(r.status === 200 && r.j.ok === true && Array.isArray(r.j.lines), "log 200 lines[]");
  ok(r.j.lines.length === 500 && r.j.offset === r.j.size && r.j.offset > 0
     && r.j.lines[0].includes("LOGTRIM-MARKER-000")
     && r.j.lines[499].includes("listening on 127.0.0.1:8765"),
     "log seeded 500 lines (R13 trim-coverage seed) + offset==size full drain");
  const r2 = await req("GET", "/api/v1/log?offset=" + r.j.offset);
  ok(r2.status === 200 && r2.j.lines.length === 0, "log incremental at EOF empty");

  /* ---- b4 layer 10 over the wire: node CAN forge Origin (a browser cannot) ---- */
  const originReq = (origin) => new Promise((res, rej) => {
    const h = { "Host": "127.0.0.1:" + port, "Connection": "close", "X-Hub-Token": token };
    if (origin !== null) h["Origin"] = origin;
    const rq = http.request({ host: "127.0.0.1", port, path: "/api/v1/health", method: "GET",
                              headers: h, timeout: 8000 },
      (resp) => { resp.resume(); resp.on("end", () => res(resp.statusCode)); });
    rq.on("error", rej);
    rq.on("timeout", () => rq.destroy(new Error("timeout")));
    rq.end();
  });
  ok((await originReq(null)) === 200, "L10 absent Origin -> 200 (curl/node/bridge shape)");
  ok((await originReq("http://127.0.0.1:" + port)) === 200, "L10 loopback Origin -> 200 (browser panel shape)");
  ok((await originReq("http://localhost:" + port)) === 200, "L10 localhost Origin -> 200");
  ok((await originReq("http://evil.com")) === 403, "L10 external Origin -> 403");
  ok((await originReq("http://127.0.0.1:9")) === 403, "L10 wrong-port Origin -> 403");
  ok((await originReq("https://127.0.0.1:" + port)) === 403, "L10 https scheme Origin -> 403");
  ok((await originReq("null")) === 403, "L10 sandboxed null Origin -> 403");

  /* ---- b4 /pending: seeded destructive ticket, masked shape, panel-face approve.
     MUST run before the live-op section: its final unmount invalidates p-ok nonces. ---- */
  r = await req("GET", "/api/v1/pending");
  ok(r.status === 200 && r.j.ok === true && r.j.count >= 1, "b4 /pending lists the seeded ticket");
  const tk = (r.j.pending || []).find((t) => t.command === "p-ok.purge");
  ok(!!tk && tk.plugin === "p-ok" && tk.risk === "destructive" && tk.state === "issued",
     "b4 pending ticket shape (plugin/command/risk/state=issued)");
  ok(tk && /^[0-9a-f]{8}$/.test(tk.nonceId), "b4 nonceId masked to first 8 hex");
  ok(tk && /^[0-9a-f]{16}$/.test(tk.argsDigest), "b4 argsDigest 16-hex (digest binding visible)");
  ok(tk && tk.remainingMs > 0 && tk.remainingMs <= 60000, "b4 remainingMs inside TTL (countdown feed)");
  ok(tk && tk.title === "Purge cache", "b4 pending carries title (dialog summary source)");
  ok(tk && typeof tk.transactionId === "string" && tk.transactionId.length > 0,
     "b4 transactionId present (approve handle)");
  r = await req("POST", "/api/v1/confirm", { transactionId: tk.transactionId, approve: true });
  ok(r.status === 200 && r.j.ok === true && r.j.state === "APPROVED",
     "b4 panel-face approve -> APPROVED (same verb the dialog button uses)");
  r = await req("GET", "/api/v1/pending");
  const tk2 = (r.j.pending || []).find((t) => t.transactionId === tk.transactionId);
  ok(!!tk2 && tk2.state === "approved", "b4 approved ticket relisted as approved (awaiting execute)");
  ok(!!tk2 && tk2.remainingMs > 60000 && tk2.remainingMs <= 600000,
     "S1b approved ticket countdown feed switches to 10-min grace window");

  /* ---- b4 version column data + update/rollback over the wire ---- */
  r = await req("GET", "/api/v1/plugins");
  const pver = r.j.plugins.find((p) => p.id === "p-ver");
  ok(pver && pver.currentVersion === "1.0.0", "b4 p-ver currentVersion 1.0.0 (pointer redirection)");
  ok(pver && Array.isArray(pver.availableVersions) &&
     pver.availableVersions.join(",") === "1.0.0,1.1.0", "b4 availableVersions menu (sorted, panel dropdown feed)");
  r = await req("POST", "/api/v1/update", { id: "p-ver", version: "1.1.0" });
  ok(r.status === 200 && r.j.ok === true && r.j.currentVersion === "1.1.0", "b4 update -> 1.1.0");
  r = await req("POST", "/api/v1/rollback", { id: "p-ver", version: "1.0.0" });
  ok(r.status === 200 && r.j.ok === true && r.j.currentVersion === "1.0.0", "b4 rollback -> 1.0.0");

  /* live op through the servant stack (mount + unmount + automount) */
  r = await req("POST", "/api/v1/mount", { id: "p-ok" });
  ok(r.status === 200 && r.j.ok === true && r.j.state === "ACTIVE" && typeof r.j.seq === "number",
     "POST mount -> ACTIVE + seq (full stack incl. executor)");
  r = await req("POST", "/api/v1/automount", { id: "p-ok", value: true });
  ok(r.status === 200 && r.j.ok === true && r.j.autoMount === true, "POST automount true");
  r = await req("GET", "/api/v1/plugins");
  const pok2 = r.j.plugins.find((p) => p.id === "p-ok");
  ok(pok2.state === "ACTIVE" && pok2.autoMount === true && pok2.wantMount === 1,
     "panel-visible state after ops (ACTIVE/autoMount/wantMount=1)");
  r = await req("POST", "/api/v1/unmount", { id: "p-ok" });
  ok(r.status === 200 && r.j.ok === true && r.j.state === "REGISTERED", "POST unmount -> REGISTERED");

  /* token gate from node too */
  r = await req("GET", "/api/v1/health", null, false);
  ok(r.status === 401, "health without token -> 401 (panel must send header)");

  console.log(`\n=== CONTRACT RESULT: ${pass} pass, ${fail} fail -> ${fail ? "FAIL" : "ALL PASS"} ===`);
  process.exit(fail ? 1 : 0);
}

console.log("usage: panel_check.mjs static|dump|contract ...");
process.exit(2);
