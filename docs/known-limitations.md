# Known limitations

This file is intentionally honest. If a limitation blocks you, open an issue with
your environment and use case.

## Platform and licensing

| Limitation | Impact | Status |
|---|---|---|
| Requires Creo Parametric **6.0** | Creo 7+ is untested/unsupported | Open (roadmap) |
| Requires a **PTC TOOLKIT license option** | Building and running plugins needs PTC entitlement | By design |
| Distributed Pro/TOOLKIT apps must be **unlocked** (`protk_unlock`) | Binaries will not run until unlocked with a TOOLKIT license | By design; see PTC CS28242 |
| Windows 7 SP1 build profile exists | Not verified on real hardware | Open |
| Clean-machine end-to-end (fresh Windows + fresh Creo) | Not verified | Mitigated by clean-room simulation + manual checklist |

## Behavior

| Limitation | Impact | Notes |
|---|---|---|
| Creo executes toolkit calls on a single main thread | Long commands block other calls; `async` gives observability, not parallelism | By design; cooperative time budget helpers exist |
| `async` jobs are polled (`GET /api/v1/jobs/<id>`) | MCP hosts that cannot poll progress see completion only | DSH integration uses a job tool |
| Dynamic tool surface | Hosts that cache tool lists may miss new plugin commands until refresh/list-changed | Host-dependent |
| Panel is ES5 and zero-dependency | No modern framework features | By design (Win7/IE11 discipline) |
| No plugin index/marketplace yet | Discovery is manual | Roadmap |
| One hub per Creo process | Multi-version coexistence not designed | Roadmap |

## Security model boundaries

- The API is loopback-only and token-authenticated; a local user who can read
  `hub_token.txt` can call it. Run Creo automation under a dedicated account.
- `safe` commands are read-only **by contract**; plugin authors are responsible
  for honoring it. Review third-party plugins before mounting.
- The panel approval drawer is the human gate for `confirm`/`destructive`; if you
  automate approval, you own the risk.

## Not planned

- Generating CAD geometry from an LLM.
- Bundling PTC SDK headers, libraries, or Creo binaries.
- Cloud telemetry from the hub DLL.
