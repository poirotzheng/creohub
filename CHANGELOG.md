# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- M5 open-source packaging: Apache-2.0 license, community files, CI, public
  snapshot generator, third-party notices, trademark policy.

## [1.0.0] - 2026-10-18

First public-facing version. Prior history is kept in the private development
repository.

### Added

- **Host plane**: `creo_hub.dll` — plugin registry, mount/unmount state machine,
  dependency reconcile, versioned pointer packages (update/rollback), audit log,
  local panel (`web/index.html`, zero external assets), loopback HTTP API with
  token + Origin/Host checks.
- **Execution plane**: `POST /api/v1/call` with schema validation, risk gate
  (`safe` / `confirm` / `destructive`), nonce-based two-step confirmation,
  `dryRun`, orphan-safe timeouts, `async:true` jobs with progress polling,
  `undoCommand`, command grouping.
- **Plugin SDK**: `sdk/hub_sdk.{c,h}` v2 — argument access, context, time budget,
  progress slot, audit hooks, active part, clean-load/unload helpers.
- **Toolchain**: `plugin_tpl` template, `plugin_init` scaffold, `plugin_manifest`
  lint, `plugin_pack` packaging (zip + SHA-256), `plugin_test_kit` self-test.
- **Distribution**: `deploy/install.cmd`, `uninstall.cmd`, `doctor.cmd`,
  `release_pack.cmd`, Inno Setup installer (Win10 full / Win7 slim profiles).
- **AI integration**: `bridge/` MCP stdio server (six management tools +
  dynamic first-class command tools), `agent/` local OpenAI-compatible assistant
  with in-panel chat tab.
- **Quality**: 30 offline gates (registry/state/reconcile/call/nonce/version/
  httpd/web_api/log/SDK/manifest/pack/installer/panel/agent/tool-contract),
  Win7 static checks, ES5 panel lint, geometry invariants.

### Known limitations

- Windows 10 x64 verified; Windows 7 build profile exists but was not verified on
  real hardware.
- Requires Creo Parametric 6.0 and a PTC TOOLKIT license option; distributed
  Pro/TOOLKIT applications must be unlocked with `protk_unlock`.
- Creo runs commands on a single main thread; `async` provides observability, not
  parallelism.
- No clean-machine (fresh Windows + fresh Creo) end-to-end run yet; a clean-room
  simulation and a manual checklist are provided instead.
