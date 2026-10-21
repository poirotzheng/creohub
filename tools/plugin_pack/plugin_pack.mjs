#!/usr/bin/env node
/**
 * tools/plugin_pack/plugin_pack.mjs —— creo-hub 接入工具链 T6 打包模块（M4a A5）
 *
 * 用途：把 plugins\<id>\ 打成一个**可分发 zip**（解压即得 plugins\<id>\），
 *       追加 dist\packages\packages.json 本地仓库索引（含 sha256），供
 *       气隙安装/升级使用（docs\39 §5.3；KiCad PCM 私有仓库先例 §4.6-10）。
 *
 * 用法：
 *   node tools/plugin_pack/plugin_pack.mjs <包目录> [--out <dist目录>] [--no-lint] [--json]
 *
 * 纪律：
 *   - 零 npm 依赖（Node 内置 zlib/crypto/fs；zip 由本文件最小实现，STORE 方法）；
 *   - **确定性**：条目按路径排序、固定 DOS 时间戳 -> 同内容同 sha256（可复现校验）；
 *   - 指针包（plugin.json 带 currentVersion）= 整包打包（含 versions\），版本取 currentVersion；
 *   - lint 默认跑（copy 流隔离根；--no-lint 跳过）；坏包=exit 1；
 *   - 只读包目录，绝不改写包内文件。
 *
 * 退出码：0=成功；1=包校验/lint/打包失败；2=用法错误。
 */
import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync, cpSync, rmSync, existsSync } from "node:fs";
import { join, resolve, basename, dirname, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..", "..");
const LINT = join(ROOT, "test", "manifest_lint.mjs");

/* ---------------------------------------------------------------- CRC32 */
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

/* ------------------------------------------------- minimal STORE zip writer */
const DOS_TIME = 0;          /* 00:00:00 */
const DOS_DATE = 0x0021;     /* 1980-01-01 (deterministic) */

function zipBuild(entries /* [{name, data}] sorted */) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const e of entries) {
    const nameBuf = Buffer.from(e.name, "utf8");
    const crc = crc32(e.data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4);            /* version needed */
    lh.writeUInt16LE(0x0800, 6);        /* UTF-8 flag */
    lh.writeUInt16LE(0, 8);             /* method: store */
    lh.writeUInt16LE(DOS_TIME, 10);
    lh.writeUInt16LE(DOS_DATE, 12);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(e.data.length, 18);
    lh.writeUInt32LE(e.data.length, 22);
    lh.writeUInt16LE(nameBuf.length, 26);
    lh.writeUInt16LE(0, 28);
    locals.push(lh, nameBuf, e.data);

    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);            /* version made by */
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0x0800, 8);
    ch.writeUInt16LE(0, 10);
    ch.writeUInt16LE(DOS_TIME, 12);
    ch.writeUInt16LE(DOS_DATE, 14);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(e.data.length, 20);
    ch.writeUInt32LE(e.data.length, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt16LE(0, 30);            /* extra */
    ch.writeUInt16LE(0, 32);            /* comment */
    ch.writeUInt16LE(0, 34);            /* disk */
    ch.writeUInt16LE(0, 36);            /* internal attrs */
    ch.writeUInt32LE(0, 38);            /* external attrs */
    ch.writeUInt32LE(offset, 42);       /* local header offset */
    centrals.push(ch, nameBuf);

    offset += 30 + nameBuf.length + e.data.length;
  }
  const cd = Buffer.concat(centrals);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(cd.length, 12);
  eocd.writeUInt32LE(offset, 16);
  eocd.writeUInt16LE(0, 20);
  return Buffer.concat([...locals, cd, eocd]);
}

/* ------------------------------------------------------------- fs helpers */
function walk(dir, out = []) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}
function readJson(p) {
  try { return JSON.parse(readFileSync(p, "utf8")); } catch { return null; }
}
function fail(msg) { console.error(`[plugin_pack] ${msg}`); process.exit(1); }

/* ------------------------------------------------------------------- main */
const argv = process.argv.slice(2);
function flag(n) { return argv.includes(n); }
function opt(n, d) { const i = argv.indexOf(n); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; }
const outOpt = opt("--out");
const pkgArg = argv.find(a => !a.startsWith("--") && a !== outOpt);
if (!pkgArg) {
  console.log("usage: plugin_pack.mjs <包目录> [--out <dist目录>] [--no-lint] [--json]");
  process.exit(2);
}
const pkgDir = resolve(pkgArg);
if (!existsSync(join(pkgDir, "plugin.json"))) fail(`plugin.json not found under ${pkgDir}`);

const root = readJson(join(pkgDir, "plugin.json"));
if (!root || typeof root.id !== "string" || !root.id) fail("plugin.json has no id");
const id = root.id;

/* pointer package: effective manifest/version from versions\<currentVersion>\ */
let version = root.version || "0.0.0";
let effective = root;
if (typeof root.currentVersion === "string" && root.currentVersion) {
  const vp = join(pkgDir, "versions", root.currentVersion, "plugin.json");
  if (!existsSync(vp)) fail(`pointer package: versions\\${root.currentVersion}\\plugin.json missing`);
  effective = readJson(vp);
  if (!effective || typeof effective.id !== "string" || effective.id !== id)
    fail("pointer package: version manifest id mismatch");
  version = effective.version || root.currentVersion;
} else if (!root.version) {
  fail("plugin.json has no version (and no currentVersion pointer)");
}

/* lint (copy-flow isolated root; same as plugin_init "generate-then-validate") */
if (!flag("--no-lint")) {
  const tmpRoot = join(ROOT, "test", "out", "pack_lint");
  rmSync(tmpRoot, { recursive: true, force: true });
  mkdirSync(join(tmpRoot, id), { recursive: true });
  cpSync(pkgDir, join(tmpRoot, id), { recursive: true });
  const r = spawnSync(process.execPath, [LINT, tmpRoot], { encoding: "utf8" });
  if (r.status !== 0) {
    console.error(r.stdout || "");
    console.error(r.stderr || "");
    fail("manifest lint FAILED (fix the package before packing)");
  }
}

/* deterministic zip: <id>/<relative-path>, sorted */
const files = walk(pkgDir)
  .map(p => ({ rel: relative(pkgDir, p).split(sep).join("/"), abs: p }))
  .sort((a, b) => (a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0));
if (!files.some(f => f.rel === "plugin.json")) fail("internal: no plugin.json in package");
const entries = files.map(f => ({ name: `${id}/${f.rel}`, data: readFileSync(f.abs) }));
const zip = zipBuild(entries);
const sha256 = createHash("sha256").update(zip).digest("hex");

/* output dir + index */
const outDir = resolve(outOpt || join(ROOT, "dist", "packages"));
mkdirSync(outDir, { recursive: true });
const zipName = `${id}-${version}.zip`;
const zipPath = join(outDir, zipName);
writeFileSync(zipPath, zip);

const indexPath = join(outDir, "packages.json");
const index = (existsSync(indexPath) ? readJson(indexPath) : null) || { schemaVersion: 1, packages: [] };
if (!Array.isArray(index.packages)) index.packages = [];
const row = {
  id, version, file: zipName, sha256, size: zip.length,
  builtAt: new Date().toISOString().slice(0, 19) + "Z",
  requires: Array.isArray(effective.requires) ? effective.requires : [],
  provides: Array.isArray(effective.provides) ? effective.provides : [],
  files: entries.length
};
index.packages = index.packages.filter(p => !(p.id === id && p.version === version));
index.packages.push(row);
index.packages.sort((a, b) => (a.id === b.id ? (a.version < b.version ? -1 : 1) : (a.id < b.id ? -1 : 1)));
writeFileSync(indexPath, JSON.stringify(index, null, 2) + "\n");

if (flag("--json")) {
  console.log(JSON.stringify({ ok: true, ...row, zipPath, indexPath }, null, 2));
} else {
  console.log(`[plugin_pack] packed ${id} ${version}`);
  console.log(`  zip    : ${zipPath} (${zip.length} bytes, ${entries.length} files)`);
  console.log(`  sha256 : ${sha256}`);
  console.log(`  index  : ${indexPath} (${index.packages.length} packages)`);
  console.log(`  install: unzip into <hub root>\\plugins\\  (=> plugins\\${id}\\ )`);
}
