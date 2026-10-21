#!/usr/bin/env node
/**
 * test/pack_check.mjs —— plugin_pack verifier (M4a A5 gate helper)
 * args: <zip> <index.json> <id> <version>
 * Asserts: zip EOCD/central directory well-formed, every entry under "<id>/",
 * "<id>/plugin.json" present, index row matches id/version/sha256/size.
 * Exit: 0 pass, 1 fail, 2 usage.
 */
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

const [zipPath, indexPath, id, version] = process.argv.slice(2);
if (!zipPath || !indexPath || !id || !version) {
  console.log("usage: pack_check.mjs <zip> <index.json> <id> <version>");
  process.exit(2);
}
let pass = 0, fail = 0;
function check(c, m) { if (c) { pass++; console.log(`[PASS] ${m}`); } else { fail++; console.log(`[FAIL] ${m}`); } }

const zip = readFileSync(zipPath);
check(zip.length > 22, "zip has bytes");

/* find EOCD (last 64KB) */
let eocd = -1;
for (let i = zip.length - 22; i >= Math.max(0, zip.length - 22 - 65535); i--) {
  if (zip.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
}
check(eocd >= 0, "EOCD signature found");
if (eocd < 0) process.exit(1);
const nEntries = zip.readUInt16LE(eocd + 10);
const cdSize = zip.readUInt32LE(eocd + 12);
const cdOff = zip.readUInt32LE(eocd + 16);
check(nEntries > 0, `EOCD entry count > 0 (${nEntries})`);
check(cdOff + cdSize <= zip.length, "central directory inside file");

/* walk central directory */
let p = cdOff;
const names = [];
let okStruct = true;
for (let i = 0; i < nEntries; i++) {
  if (p + 46 > zip.length || zip.readUInt32LE(p) !== 0x02014b50) { okStruct = false; break; }
  const method = zip.readUInt16LE(p + 10);
  const crc = zip.readUInt32LE(p + 16);
  const csize = zip.readUInt32LE(p + 20);
  const nameLen = zip.readUInt16LE(p + 28);
  const extraLen = zip.readUInt16LE(p + 30);
  const commentLen = zip.readUInt16LE(p + 32);
  const lho = zip.readUInt32LE(p + 42);
  const name = zip.subarray(p + 46, p + 46 + nameLen).toString("utf8");
  names.push({ name, crc, csize, lho, method });
  p += 46 + nameLen + extraLen + commentLen;
}
check(okStruct, "all central directory records well-formed");
check(p === cdOff + cdSize, "central directory size matches");
check(names.every(n => n.name.startsWith(`${id}/`)), `every entry rooted at ${id}/`);
check(names.some(n => n.name === `${id}/plugin.json`), `${id}/plugin.json present`);
check(names.every(n => n.method === 0), "STORE method (deterministic, zero-dep)");

/* local header + data integrity for each entry */
let dataOk = true;
for (const n of names) {
  if (zip.readUInt32LE(n.lho) !== 0x04034b50) { dataOk = false; break; }
  const nl = zip.readUInt16LE(n.lho + 26);
  const el = zip.readUInt16LE(n.lho + 28);
  const start = n.lho + 30 + nl + el;
  if (start + n.csize > zip.length) { dataOk = false; break; }
}
check(dataOk, "local headers/data ranges valid");

/* index row */
const index = JSON.parse(readFileSync(indexPath, "utf8"));
check(index.schemaVersion === 1 && Array.isArray(index.packages), "index schema v1");
const row = index.packages.find(r => r.id === id && r.version === version);
check(!!row, `index has ${id}@${version}`);
const sha = createHash("sha256").update(zip).digest("hex");
check(row && row.sha256 === sha, "index sha256 matches zip bytes");
check(row && row.size === zip.length, "index size matches zip bytes");
check(row && row.file === `${id}-${version}.zip`, "index file name");
check(row && Array.isArray(row.requires), "index carries requires[]");

console.log(`\n=== PACK CHECK: ${pass} pass, ${fail} fail ===`);
process.exit(fail ? 1 : 0);
