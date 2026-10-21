# Third-party notices

creo-hub is licensed under the Apache License 2.0 (see `LICENSE`). Documentation
is licensed under CC BY 4.0 (see `docs/LICENSE`). The following third-party
components are used by, or are required to build, this project.

## Bundled in this repository (source form)

| Component | Version | License | Location | Notes |
|---|---|---|---|---|
| cJSON | 1.7.x (vendored) | MIT | `src/hub/vendor/cjson/` | Copyright (c) 2009-2017 Dave Gamble and cJSON contributors. See `src/hub/vendor/cjson/LICENSE` or the header of `cJSON.c`. |

## Consumed at install/build time (not redistributed in this repository)

| Component | Version | License | Notes |
|---|---|---|---|
| `@modelcontextprotocol/sdk` | 1.30.0 (npm) | MIT | MCP stdio bridge (`bridge/`). Installed by `npm ci`; not committed (`node_modules/` ignored). |
| Inno Setup | 6.3.3 | Inno Setup License | Build-time installer compiler only. The generated `setup.exe` is a combined work; the compiler itself is not distributed in this repository. |
| PTC Creo Parametric 6.0 / Pro/TOOLKIT SDK | 6.0 | Proprietary (PTC) | **Not distributed.** Users must obtain Creo and a TOOLKIT license option themselves. Headers and import libraries are never committed. Distribution of built Pro/TOOLKIT applications is subject to `protk_unlock` and PTC license terms. |
| Node.js | ≥ 18 | MIT | Runtime for optional bridge/agent/panel tooling; not vendored in this repository (portable Node may be bundled in release artifacts only). |
| Chromium / Edge | any recent | BSD-style | Optional; used only by the panel E2E gate (`test/run_panel_e2e.cmd`). |

## Trademarks

Creo® is a trademark of PTC Inc. creo-hub is an independent open-source project
and is not affiliated with, endorsed by, or sponsored by PTC Inc. See
`TRADEMARKS.md`.

## Reporting

If you believe a third-party notice is missing or incorrect, please open an
issue or use the security contact in `SECURITY.md`.
