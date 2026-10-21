# Gate catalog

All gates run on Windows. `test\run_all.cmd` runs the offline suite (no Creo
required). Live gates need a running Creo session and are listed separately.

## Offline gates (run_all)

| Gate | Needs | What it proves |
|---|---|---|
| `run_registry_test` | MSVC, PTC headers | Scan/validate, seven-state machine, state persistence, mount/unmount, automount, E12 TAINTED |
| `run_reconcile_test` | MSVC, PTC headers | Dependency graph reconcile, topological order, drain-on-unmount |
| `run_depgraph_test` | MSVC | Cycle detection / requires edges |
| `run_marsh_test` | MSVC, PTC headers | Argument marshaling + token handling |
| `run_nonce_test` | MSVC, PTC headers | Nonce slots, TTL, eviction, auto-pair |
| `run_version_test` | MSVC, PTC headers | Pointer package resolution, update/rollback |
| `run_log_test` | MSVC, PTC headers | Audit log rotation + JSON schema |
| `run_sdk_test` | MSVC, PTC headers | hub_sdk v2 argument access / budget / progress |
| `run_httpd_test` | MSVC, PTC headers | HTTP layer, token, Origin/Host checks |
| `run_v2_manifest_test` | MSVC, PTC headers | Manifest v2 C-face contract |
| `run_call_test` | MSVC, PTC headers | /call validation, risk gate, dry-run, orphan timeout |
| `run_web_api_test` | MSVC, PTC headers | Web API v1 endpoints, async job semantics |
| `run_p12_check` | MSVC, PTC headers | P12 artifact (v2 demo plugin) contract |
| `run_p3_geom_check` | MSVC, PTC headers | demo3 geometry invariants |
| `run_tool_contract_test` | Node | Bridge tool-face golden (names/descriptions) |
| `run_hooks_gate_test` | Node | DSH PreToolUse ask-gate patch behavior |
| `run_install_kernel_test` | cmd only | Installer kernel: install/register/idempotent/negative/uninstall |
| `run_plugin_tpl_check` | MSVC, PTC headers | Template compiles + D-W7 static checks |
| `run_plugin_init_check` | Node | Scaffold generates six modules + requires/provides |
| `run_manifest_lint` | Node | Manifest lint vs dual-face golden (23 fixtures) |
| `run_pack_test` | Node | plugin_pack zip + index + deterministic sha256 + negatives |
| `run_agent_test` | Node | Local assistant loop (mock hub + fake LLM), 19 asserts |
| `run_bridge_battery` | Node, MCP SDK | Bridge offline battery (117 asserts) |
| `run_bridge_preflight` | Node, MCP SDK | Bridge preflight (env/token/version pin) |
| `run_dev_face_test` | MSVC | DEV-face regression (CREO_HUB_DEV builds) |

## Live gates (need Creo)

| Gate | Needs | What it proves |
|---|---|---|
| `run_e11_probe` | Creo + dev hub | Channel probe / toolkit task execution |
| `run_panel_e2e` | Creo hub running + Chrome/Edge | Panel static ES5, live API contract, rendered DOM |
| `run_real_dump` | Creo hub running + browser | Headless DOM dump for panel diagnostics |
| `run_installer_e2e_test` | Inno Setup + writable TEMP/HKCU | Installer build → silent install → assert → uninstall |
| `run_plugin_selftest <pkg>` | MSVC, PTC headers | One-command plugin acceptance (compile/lint/assert) |

## CI

`.github/workflows/ci.yml` runs the Node-only subset (`run_manifest_lint`,
`run_pack_test`, `run_agent_test`) plus hygiene. The C build needs the
proprietary PTC SDK and is intentionally excluded from hosted runners.

## Environment

- `CREO_HUB_OUT` redirects compiled objects/exes (default `test\out`).
  In restricted workspaces use `%TEMP%\creo-hub-test-out`.
- `CREO_TOOLKIT_ROOT` (or `local.env`) points at the Pro/TOOLKIT SDK.
- `CI=1` suppresses interactive `pause` prompts.
