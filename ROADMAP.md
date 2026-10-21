# Roadmap

This roadmap lists work that is **not** in the v1.0 public release. Items are
triggered by real demand, not dates.

## Under consideration

| Item | Trigger | Notes |
|---|---|---|
| P3 key-level service registry (`injects` / `hub_provide`) | A plugin needs to call another plugin's service | Design shape documented in `docs/14`; not implemented. |
| Cooperative yielding during long commands | Real parallel-workload demand with a main-thread safety argument | Creo's main thread is single; async currently provides observability only. |
| jlink / weblink / macro / config adapters | A real customer asset requires them | Adapter bus is already generalized; jlink API verified. |
| Plugin index JSON for discovery | Community asks for a machine-readable plugin catalog | KiCad-style `versions[] + sha256`; `plugin_pack` already emits SHA-256. |
| Windows 7 real-hardware verification | A maintainer or community member has Win7 SP1 x64 + Creo 6 | Static gates (w7_check, ES5) pass today. |
| Creo 7+ support | User demand | Requires TOOLKIT header/API compatibility work. |
| Clean-machine end-to-end validation | A fresh Windows + fresh Creo environment is available | Clean-room simulation + manual checklist exist instead. |
| Multi-hub / multi-version coexistence | Deployment scenario with several Creo versions on one machine | Version-pin handshake exists; discovery strategy needs work. |

## Not planned

- Generating geometry from an LLM (the model only calls deterministic commands).
- Bundling PTC TOOLKIT headers/libraries or Creo binaries.
- Telemetry or cloud calls from the hub DLL.

## How to influence the roadmap

Open an issue with a concrete use case, environment, and what you tried.
Commercial/enterprise needs: `creohubsecurity@163.com`.
