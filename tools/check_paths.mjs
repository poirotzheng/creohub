#!/usr/bin/env node
/**
 * check_paths.mjs — local-path and secret hygiene scanner.
 *
 * Scans the working tree for machine-specific paths, user names, and common
 * secret shapes. The public snapshot must be clean; the private development
 * tree is expected to have hits (historical notes), so the default mode only
 * reports. Use --strict (CI / release gate) to fail on any hit.
 *
 * Usage:
 *   node tools/check_paths.mjs [--strict] [--json] [--root <dir>]
 *
 * Exit: 0 = no hits (or report mode), 1 = hits under --strict, 2 = IO error.
 */

import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { join, resolve, extname, relative } from 'node:path';
import os from 'node:os';

const args = process.argv.slice(2);
const STRICT = args.includes('--strict');
const JSON_OUT = args.includes('--json');
const rootIdx = args.indexOf('--root');
const ROOT = rootIdx >= 0 ? resolve(args[rootIdx + 1]) : process.cwd();

const SKIP_DIRS = new Set([
  '.git',
  'node_modules',
  'dist',
  'logs',
  'out',
  '.dsh',
  '.dsh_xb1',
  'outputs',
  '.acl-recovery',
  '.agents',
  '.npm-cache',
]);

const TEXT_EXT = new Set([
  '.md', '.txt', '.c', '.h', '.def', '.mjs', '.js', '.json', '.yml', '.yaml',
  '.cmd', '.bat', '.ps1', '.iss', '.html', '.css', '.csv', '.example', '.env',
]);
const TEXT_NAMES = new Set(['NOTICE', 'LICENSE', 'VERSION', '.gitignore', '.gitattributes']);

const MAX_BYTES = 2 * 1024 * 1024;

function escapeRe(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/** Match a literal Windows path whether backslashes are single or doubled. */
function flexPath(text) {
  return escapeRe(text).replace(/\\\\/g, '[\\\\/]{1,2}');
}

/** Optional private hints (never shipped): one literal path per line. */
function loadHints() {
  const file = join(ROOT, 'tools', 'path-hints.txt');
  if (!existsSync(file)) return [];
  return readFileSync(file, 'utf8')
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'))
    .map((l) => new RegExp(flexPath(l), 'gi'));
}

const HOME = os.homedir();
const HINT_RULES = loadHints().map((re, i) => ['configured private path #' + (i + 1), re]);

/** [label, regex] — order matters for reporting */
const RULES = [
  ['user home path', new RegExp(flexPath(HOME), 'gi')],
  ['Windows user profile path (C:\\Users\\<name>)', /C:\\+Users\\+(?!<|Public\b)[^\\\s"']+/gi],
  ...HINT_RULES,
  ['PTC toolkit path hardcoded', /E:[\\/]{1,2}Program Files[\\/]{1,2}PTC[^"'\r\n]*/gi],
  ['OpenAI-style key', /\bsk-[A-Za-z0-9_-]{20,}/g],
  ['GitHub token', /\bghp_[A-Za-z0-9]{20,}/g],
  ['AWS access key', /\bAKIA[0-9A-Z]{16}\b/g],
  ['private key block', /-----BEGIN (?:RSA |OPENSSH |EC |DSA )?PRIVATE KEY-----/g],
];

const hits = [];
const scanned = { files: 0, bytes: 0, skipped: 0 };

function walk(dir) {
  let entries;
  try {
    entries = readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const entry of entries) {
    const full = join(dir, entry.name);
    if (entry.isSymbolicLink()) {
      scanned.skipped++;
      continue;
    }
    if (entry.isDirectory()) {
      if (SKIP_DIRS.has(entry.name)) {
        scanned.skipped++;
        continue;
      }
      walk(full);
      continue;
    }
    if (!entry.isFile()) continue;
    const ext = extname(entry.name).toLowerCase();
    if (!TEXT_EXT.has(ext) && !TEXT_NAMES.has(entry.name)) {
      scanned.skipped++;
      continue;
    }
    let st;
    try {
      st = statSync(full);
    } catch {
      scanned.skipped++;
      continue;
    }
    if (st.size > MAX_BYTES) {
      scanned.skipped++;
      continue;
    }
    let text;
    try {
      text = readFileSync(full, 'utf8');
    } catch {
      scanned.skipped++;
      continue;
    }
    scanned.files++;
    scanned.bytes += st.size;
    const lines = text.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
      for (const [label, re] of RULES) {
        re.lastIndex = 0;
        if (re.test(lines[i])) {
          hits.push({
            file: relative(ROOT, full).replace(/\\/g, '/'),
            line: i + 1,
            rule: label,
            text: lines[i].trim().slice(0, 160),
          });
        }
      }
    }
  }
}

walk(ROOT);

const byFile = new Map();
for (const h of hits) {
  byFile.set(h.file, (byFile.get(h.file) || 0) + 1);
}

if (JSON_OUT) {
  console.log(
    JSON.stringify(
      {
        root: ROOT,
        strict: STRICT,
        ok: hits.length === 0,
        scanned,
        hitCount: hits.length,
        files: [...byFile.entries()].map(([file, count]) => ({ file, count })),
        hits,
      },
      null,
      2
    )
  );
} else {
  console.log('[i] root = ' + ROOT);
  console.log('[i] scanned ' + scanned.files + ' text files (' + Math.round(scanned.bytes / 1024) + ' KB), skipped ' + scanned.skipped);
  if (hits.length === 0) {
    console.log('=== PATH/SECRET SCAN: CLEAN ===');
  } else {
    console.log('[!] ' + hits.length + ' hit(s) in ' + byFile.size + ' file(s):');
    for (const [file, count] of [...byFile.entries()].sort()) {
      console.log('    ' + file + '  (' + count + ')');
    }
    const shown = hits.slice(0, 30);
    for (const h of shown) {
      console.log('    - ' + h.file + ':' + h.line + '  [' + h.rule + ']  ' + h.text);
    }
    if (hits.length > shown.length) {
      console.log('    ... ' + (hits.length - shown.length) + ' more (use --json for the full list)');
    }
    console.log(
      STRICT
        ? '=== PATH/SECRET SCAN: FAIL (strict) ==='
        : '=== PATH/SECRET SCAN: HITS (report mode; use --strict to fail) ==='
    );
  }
}

process.exit(hits.length > 0 && STRICT ? 1 : 0);
