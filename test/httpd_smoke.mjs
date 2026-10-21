/* creo-hub M2 gate O3 (second, external real client): Node raw-socket
 * smoke against a running httpd_test.exe --serve instance.
 * Usage: node httpd_smoke.mjs <port>
 * Exit 0 = ALL PASS. Deliberately uses net.Socket (not fetch/http) so the
 * request bytes are hand-controlled, exactly like the C test client. */
import net from 'node:net';

const port = Number(process.argv[2] || 18766);
let fails = 0;
let passes = 0;

function check(ok, name) {
  console.log((ok ? '[PASS] ' : '[FAIL] ') + name);
  if (ok) passes++; else fails++;
}

function raw(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let done = false;
    const s = net.connect(port, '127.0.0.1', () => s.end(req)); // write + half-close
    s.on('data', (d) => chunks.push(d));
    const finish = () => { if (!done) { done = true; resolve(Buffer.concat(chunks).toString('latin1')); } };
    s.on('end', finish);
    s.on('close', finish);
    s.setTimeout(5000, () => { s.destroy(); reject(new Error('client timeout')); });
    s.on('error', reject);
  });
}

const status = (r) => (r.startsWith('HTTP/1.1 ') ? parseInt(r.slice(9, 12), 10) : -1);

const idx = await raw(`GET / HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nConnection: close\r\n\r\n`);
check(status(idx) === 200, 'node GET / -> 200');
check(idx.includes('hub-httpd-smoke-ok'), 'node GET / body contains smoke marker');
check(!idx.includes('Access-Control'), 'node response has no CORS headers (L5)');
check(!idx.includes('Set-Cookie'), 'node response has no cookies (L5)');

const okHost = await raw(`GET /index.html HTTP/1.1\r\nHost: LOCALHOST\r\n\r\n`);
check(status(okHost) === 200, 'node Host LOCALHOST (no port, case-insensitive) -> 200');

const evil = await raw(`GET / HTTP/1.1\r\nHost: evil.example.com\r\n\r\n`);
check(status(evil) === 403, 'node Host evil.example.com -> 403 (L2)');

const wrongPort = await raw(`GET / HTTP/1.1\r\nHost: 127.0.0.1:${port + 1}\r\n\r\n`);
check(status(wrongPort) === 403, 'node Host with wrong port -> 403 (L2)');

const nf = await raw(`GET /definitely-missing HTTP/1.1\r\nHost: localhost:${port}\r\n\r\n`);
check(status(nf) === 404, 'node unknown path -> 404 (L7 whitelist)');

const trav = await raw(`GET /../secret.txt HTTP/1.1\r\nHost: localhost:${port}\r\n\r\n`);
check(status(trav) === 400, 'node traversal -> 400 (L7)');

/* M3c c1 (ISSUE-c1-1): b4 Origin ruling (docs\25 b4, user-decreed loopback
 * whitelist) - a non-loopback Origin is rejected 403 BEFORE verb routing,
 * so the old "evil Origin OPTIONS -> 405" expectation is stale. Split into
 * the two post-ruling semantics: evil Origin -> 403 (L10 precedes O9),
 * loopback Origin -> 405 verb gate (original O9 intent), neither with CORS. */
const opt = await raw(`OPTIONS /api/v1/plugins HTTP/1.1\r\nHost: localhost:${port}\r\nOrigin: http://evil.example.com\r\n\r\n`);
check(status(opt) === 403 && !opt.includes('Access-Control'), 'node OPTIONS + evil Origin -> 403 (b4 L10 precedes verb gate), still no CORS');

const optOk = await raw(`OPTIONS /api/v1/plugins HTTP/1.1\r\nHost: localhost:${port}\r\nOrigin: http://127.0.0.1:${port}\r\n\r\n`);
check(status(optOk) === 405 && !optOk.includes('Access-Control'), 'node OPTIONS + loopback Origin -> 405 verb gate (O9), still no CORS');

const garbage = await raw(`XYZZY PLUGH\r\n\r\n`);
check(status(garbage) === 400, 'node garbage request line -> 400');

console.log(`\n=== NODE SMOKE: ${passes} pass, ${fails} fail -> ${fails ? 'FAIL' : 'ALL PASS'} ===`);
process.exit(fails ? 1 : 0);
