#!/usr/bin/env node
/**
 * replace_placeholders.mjs — fill or verify release placeholders.
 *
 * Placeholders used before publication:
 *   <COPYRIGHT HOLDER>  — license/NOTICE/docs
 *   <ORG>               — GitHub organization/user in README/badges
 *   <CONTACT>           — security/support/CoC contact
 *
 * Usage:
 *   node tools/replace_placeholders.mjs --holder "ACME Ltd." --org acme --contact security@example.com
 *   node tools/replace_placeholders.mjs --check
 *
 * Options: --root <dir> (default: repository root), --check, --json
 * Exit: 0 = ok, 1 = placeholders remain (check mode), 2 = usage/IO error.
 */

import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { extname, join, relative, resolve } from 'node:path';

const args = process.argv.slice(2);
const JSON_OUT = args.includes('--json');
const CHECK = args.includes('--check');
const rootIdx = args.indexOf('--root');
const ROOT = rootIdx >= 0 ? resolve(args[rootIdx + 1]) : process.cwd();

function opt(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : null;
}

const HOLDER = opt('--holder');
const ORG = opt('--org');
const CONTACT = opt('--contact');

if (!CHECK && (!HOLDER || !ORG || !CONTACT)) {
  console.error('usage: --holder <name> --org <org> --contact <email> | --check');
  process.exit(2);
}

const SKIP_DIRS = new Set(['.git', 'node_modules', 'dist', 'logs', 'out', 'build', 'test']);
const TEXT_EXT = new Set([
  '.md', '.txt', '.c', '.h', '.def', '.mjs', '.js', '.json', '.yml', '.yaml',
  '.cmd', '.bat', '.ps1', '.iss', '.html', '.css',
]);
const TEXT_NAMES = new Set(['NOTICE', 'LICENSE', 'VERSION', '.gitignore', '.gitattributes']);
const SKIP_FILES = new Set(['tools/replace_placeholders.mjs']);

const TOKENS = ['<COPYRIGHT HOLDER>', '<ORG>', '<CONTACT>'];
const found = [];
const changed = [];

function walk(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      if (!SKIP_DIRS.has(entry.name)) walk(full);
      continue;
    }
    if (!entry.isFile()) continue;
    const ext = extname(entry.name).toLowerCase();
    if (!TEXT_EXT.has(ext) && !TEXT_NAMES.has(entry.name)) continue;
    const rel = relative(ROOT, full).replace(/\\/g, '/');
    if (SKIP_FILES.has(rel)) continue;
    const st = statSync(full);
    if (st.size > 2 * 1024 * 1024) continue;
    let text = readFileSync(full, 'utf8');
    const hits = TOKENS.filter((t) => text.includes(t));
    if (!hits.length) continue;
    found.push({ file: rel, tokens: hits });
    if (!CHECK) {
      if (HOLDER) text = text.split('<COPYRIGHT HOLDER>').join(HOLDER);
      if (ORG) text = text.split('<ORG>').join(ORG);
      if (CONTACT) text = text.split('<CONTACT>').join(CONTACT);
      writeFileSync(full, text, 'utf8');
      changed.push(rel);
    }
  }
}

walk(ROOT);

const ok = found.length === 0;

if (JSON_OUT) {
  console.log(JSON.stringify({ ok, check: CHECK, root: ROOT, found, changed }, null, 2));
} else {
  if (found.length === 0) {
    console.log('=== PLACEHOLDERS: NONE ===');
  } else {
    console.log('[i] ' + found.length + ' file(s) contain placeholders:');
    for (const f of found) console.log('    ' + f.file + '  ' + f.tokens.join(' '));
    if (!CHECK) console.log('[i] replaced in ' + changed.length + ' file(s)');
    else console.log('=== PLACEHOLDERS: REMAIN (release gate fails) ===');
  }
}

process.exit(CHECK && !ok ? 1 : 0);
