# Skills

This directory ships agent skills for [DeepSeek Harness (DSH)](https://github.com/deepseek-ai/deepseek-harness)
and other skill-aware hosts.

| Skill | Purpose |
|---|---|
| `creo-hub-plugin-onboarding/` | End-to-end pipeline for onboarding a Creo Pro/TOOLKIT plugin: manifest contract, `hub_sdk` command face, `/call` execution, panel approval, bridge tool surface, async jobs, packaging (zip + SHA-256), install/uninstall/doctor, versioned hot-swap, dependency reconciliation. |

## Use with DSH

Add this directory (or its parent) as a skill scan root, or copy the skill folder
into one of DSH's scan roots (`<project>/.dsh/skills`, `<project>/.agents/skills`,
`$DSH_HOME/skills`). DSH discovers `<root>/<name>/SKILL.md` and hot-reloads.

## Sync discipline

The skill, the developer guide and the plugin template must not drift. Run:

```bat
node tools\check_skill_sync.mjs
```

It verifies the skill frontmatter, the referenced repository paths, and the core
contract anchors in the developer guide.
