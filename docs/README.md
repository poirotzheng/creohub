# Documentation

| Document | Audience | Language |
|---|---|---|
| [User guide](40_creohub安装与使用教程.md) | End users / IT | Chinese |
| [Developer guide](18_插件开发者指南.md) | Plugin developers | Chinese |
| [Known limitations](known-limitations.md) | Everyone | English |
| [Roadmap](../ROADMAP.md) | Everyone | English |
| [Changelog](../CHANGELOG.md) | Everyone | English |
| [Security policy](../SECURITY.md) | Everyone | English |

## Architecture (summary)

creo-hub has three planes:

1. **Host plane** — `creo_hub.dll`, loaded by Creo at startup. Owns the plugin
   registry, state machine, dependency reconciliation, versioned pointer
   packages, HTTP API, audit log and the panel. Never unloaded.
2. **Component plane** — business plugins (Pro/TOOLKIT DLLs) mounted/unmounted at
   runtime. Each declares commands in `plugin.json` v2 with schema and risk.
3. **Model plane** — MCP bridge and local agent outside Creo. They only call
   declared deterministic commands; the hub DLL never talks to an LLM.

## Copyright and license

Documentation: CC BY 4.0 (see [LICENSE](LICENSE)). Source code: Apache-2.0
(repository root `LICENSE`).

Creo® is a trademark of PTC Inc. This project is independent and not affiliated
with PTC Inc.
