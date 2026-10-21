#!/usr/bin/env node
/**
 * check_skill_sync.mjs — guard the skill / guide / template from drifting apart.
 *
 * Checks:
 *   - skills/<name>/SKILL.md exists, frontmatter name matches the folder, and
 *     name/description are present;
 *   - referenced repository paths exist (docs/18, tools/plugin_init,
 *     tools/plugin_pack, sdk/hub_sdk.h, tools/plugin_tpl);
 *   - the developer guide still contains the core contract anchors
 *     (plugin.json, risk, inputSchema, descForLLM, hub_cmd).
 *
 * Exit: 0 = in sync, 1 = drift, 2 = usage/IO error.
 */

import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const SKILLS = join(ROOT, 'skills');

function fail(msg) {
  console.error('[FAIL] ' + msg);
  process.exitCode = 1;
}

if (!existsSync(SKILLS)) {
  console.error('[ERR] skills/ not found');
  process.exit(2);
}

const dirs = readdirSync(SKILLS, { withFileTypes: true })
  .filter((e) => e.isDirectory())
  .map((e) => e.name);

if (!dirs.length) fail('no skill directories under skills/');

for (const name of dirs) {
  const skillPath = join(SKILLS, name, 'SKILL.md');
  if (!existsSync(skillPath)) {
    fail(name + ': missing SKILL.md');
    continue;
  }
  const text = readFileSync(skillPath, 'utf8');
  const fm = text.match(/^---\r?\n([\s\S]*?)\r?\n---/);
  if (!fm) {
    fail(name + ': missing frontmatter');
    continue;
  }
  const field = (key) => {
    const m = fm[1].match(new RegExp('^' + key + ':\\s*(.+)$', 'm'));
    return m ? m[1].trim().replace(/^["']|["']$/g, '') : '';
  };
  const declared = field('name');
  if (declared !== name) fail(name + ': frontmatter name "' + declared + '" != folder');
  if (!field('description')) fail(name + ': missing description');
}

const requiredPaths = [
  'docs/18_插件开发者指南.md',
  'sdk/hub_sdk.h',
  'tools/plugin_init/plugin_init.mjs',
  'tools/plugin_pack/plugin_pack.mjs',
  'tools/plugin_tpl/plugin.json',
];
for (const rel of requiredPaths) {
  if (!existsSync(join(ROOT, rel))) fail('missing referenced path: ' + rel);
}

const guidePath = join(ROOT, 'docs/18_插件开发者指南.md');
if (existsSync(guidePath)) {
  const guide = readFileSync(guidePath, 'utf8');
  for (const anchor of ['plugin.json', 'risk', 'inputSchema', 'descForLLM', 'hub_cmd']) {
    if (!guide.includes(anchor)) fail('developer guide lost anchor: ' + anchor);
  }
}

if (process.exitCode) {
  console.log('=== SKILL SYNC: FAIL ===');
} else {
  console.log('[i] skills: ' + dirs.join(', '));
  console.log('=== SKILL SYNC: OK ===');
}
