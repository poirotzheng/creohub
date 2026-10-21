#!/usr/bin/env node
/**
 * check_version.mjs — single-source version consistency gate.
 *
 * VERSION at the repository root is the source of truth. This script asserts
 * that the version copies used by the build/release pipeline match it.
 *
 * Usage:
 *   node tools/check_version.mjs            # check
 *   node tools/check_version.mjs --json     # machine-readable output
 *
 * Exit: 0 = consistent, 1 = mismatch, 2 = usage/IO error.
 */

import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const JSON_OUT = process.argv.includes('--json');

function read(rel) {
  return readFileSync(join(ROOT, rel), 'utf8');
}

function fail(msg) {
  if (JSON_OUT) {
    console.log(JSON.stringify({ ok: false, error: msg }, null, 2));
  } else {
    console.error('[ERR] ' + msg);
  }
  process.exit(2);
}

let version;
try {
  version = read('VERSION').trim();
} catch (err) {
  fail('cannot read VERSION: ' + err.message);
}

if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) {
  fail('VERSION is not semver: ' + JSON.stringify(version));
}

/** [file, description, extractor returning the version it declares] */
const CHECKS = [
  [
    'src/hub/web_api.c',
    'HUB_VERSION_STR',
    (text) => {
      const m = text.match(/#define\s+HUB_VERSION_STR\s+"([^"]+)"/);
      return m && m[1];
    },
  ],
  [
    'deploy/installer/setup.iss',
    'AppVer',
    (text) => {
      const m = text.match(/#define\s+AppVer\s+"([^"]+)"/);
      return m && m[1];
    },
  ],
  [
    'deploy/installer/build_installer.cmd',
    'VER default',
    (text) => {
      const m = text.match(/set\s+"VER=([^"]+)"/i);
      return m && m[1];
    },
  ],
  [
    'deploy/release_pack.cmd',
    'VER default',
    (text) => {
      const m = text.match(/set\s+"VER=([^"]+)"/i);
      return m && m[1];
    },
  ],
  [
    'CHANGELOG.md',
    'top released version',
    (text) => {
      const m = text.match(/^##\s+\[(\d+\.\d+\.\d+)\]/m);
      return m && m[1];
    },
  ],
];

const results = [];
let failures = 0;

for (const [file, label, extract] of CHECKS) {
  let text;
  try {
    text = read(file);
  } catch (err) {
    results.push({ file, label, declared: null, ok: false, error: 'unreadable' });
    failures++;
    continue;
  }
  const declared = extract(text);
  const ok = declared === version;
  results.push({ file, label, declared: declared || null, ok });
  if (!ok) failures++;
}

if (JSON_OUT) {
  console.log(JSON.stringify({ ok: failures === 0, version, results }, null, 2));
} else {
  console.log('[i] VERSION = ' + version);
  for (const r of results) {
    console.log(
      (r.ok ? '  OK   ' : '  FAIL ') +
        r.file +
        ' (' +
        r.label +
        ') = ' +
        (r.declared === null ? '<missing>' : r.declared)
    );
  }
  console.log(failures === 0 ? '=== VERSION GATE: PASS ===' : '=== VERSION GATE: FAIL (' + failures + ') ===');
}

process.exit(failures === 0 ? 0 : 1);
