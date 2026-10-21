#!/usr/bin/env node
/**
 * verify_release.mjs — verify a release_pack output directory.
 *
 * Checks: required artifacts exist and are non-trivial; SHA256SUMS.txt lists
 * basenames only (no local paths) and every hash matches; MANIFEST files exist.
 *
 * Usage: node deploy/verify_release.mjs [--dist <dir>]
 * Exit: 0 = ok, 1 = verification failed, 2 = usage/IO error.
 */

import { existsSync, readFileSync, statSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { basename, join, resolve } from 'node:path';

const args = process.argv.slice(2);
const distIdx = args.indexOf('--dist');
const ROOT = resolve(process.cwd());
const DIST = distIdx >= 0 ? resolve(args[distIdx + 1]) : join(ROOT, 'dist');

function die(msg) {
  console.error('[ERR] ' + msg);
  process.exit(2);
}

if (!existsSync(join(ROOT, 'VERSION'))) die('VERSION not found (run from repo root)');
const version = readFileSync(join(ROOT, 'VERSION'), 'utf8').trim();
if (!existsSync(DIST)) die('dist directory not found: ' + DIST);

const problems = [];
const expected = [
  `creo-hub-${version}-setup.exe`,
  `creo-hub-${version}-win10.zip`,
  `creo-hub-${version}-win7.zip`,
  'SHA256SUMS.txt',
  `creo-hub-${version}-win10.MANIFEST.txt`,
  `creo-hub-${version}-win7.MANIFEST.txt`,
];

function sha256(file) {
  return createHash('sha256').update(readFileSync(file)).digest('hex');
}

for (const name of expected) {
  const p = join(DIST, name);
  if (!existsSync(p)) {
    problems.push('missing: ' + name);
    continue;
  }
  const size = statSync(p).size;
  if (name.endsWith('.zip') && size < 1024 * 1024) problems.push('zip too small: ' + name + ' (' + size + ')');
  if (name.endsWith('.exe') && size < 1024 * 1024) problems.push('setup too small: ' + name + ' (' + size + ')');
}

const sumsPath = join(DIST, 'SHA256SUMS.txt');
if (existsSync(sumsPath)) {
  const lines = readFileSync(sumsPath, 'utf8').split(/\r?\n/).filter(Boolean);
  const seen = new Set();
  for (const line of lines) {
    const m = line.match(/^([0-9a-fA-F]{64})\s{2}(.+)$/);
    if (!m) {
      problems.push('bad SHA256SUMS line: ' + line.slice(0, 80));
      continue;
    }
    const [, hash, name] = m;
    if (name !== basename(name) || name.includes(':') || name.includes('/') || name.includes('\\')) {
      problems.push('SHA256SUMS leaks a path (must be a basename): ' + name);
      continue;
    }
    seen.add(name);
    const p = join(DIST, name);
    if (!existsSync(p)) {
      problems.push('SHA256SUMS references missing file: ' + name);
      continue;
    }
    const actual = sha256(p);
    if (actual.toLowerCase() !== hash.toLowerCase()) problems.push('hash mismatch: ' + name);
  }
  for (const name of ['creo-hub-' + version + '-win10.zip', 'creo-hub-' + version + '-win7.zip', 'creo-hub-' + version + '-setup.exe']) {
    if (!seen.has(name)) problems.push('SHA256SUMS missing entry: ' + name);
  }
}

// A release must not contain local machine paths in text artifacts.
for (const name of ['SHA256SUMS.txt']) {
  const p = join(DIST, name);
  if (!existsSync(p)) continue;
  const text = readFileSync(p, 'utf8');
  if (/[A-Za-z]:\\/.test(text) || text.includes('Users\\')) problems.push(name + ' contains an absolute path');
}

const files = readdirSync(DIST).filter((f) => !f.endsWith('.MANIFEST.txt') && f !== 'SHA256SUMS.txt');
console.log('[i] dist = ' + DIST + ' (version ' + version + ')');
console.log('[i] artifacts: ' + files.join(', '));
if (problems.length) {
  console.log('=== RELEASE VERIFY: FAIL (' + problems.length + ') ===');
  for (const p of problems) console.log('  - ' + p);
  process.exit(1);
}
console.log('=== RELEASE VERIFY: OK ===');
