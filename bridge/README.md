# creo-hub MCP bridge (creo-hub-bridge)

A standard **stdio MCP server** that exposes creo-hub to DSH, Claude Desktop,
Cursor, VS Code, or any MCP host. It is stateless: it discovers the hub port and
reads the token on every call, so hub restarts or token rotation need no
reconfiguration.

The hub DLL is not modified by the bridge. The bridge opens no listening port.

## Quick start

```bat
cd <REPO_ROOT>\bridge
npm install
cd ..
test\run_bridge_battery.cmd
```

Only dependency: `@modelcontextprotocol/sdk` (pinned in `package-lock.json`).
Node.js 18+ is required.

## Configuration

| Env var | Meaning |
|---|---|
| `CREO_HUB_ROOT` | Absolute path of the creo-hub root (must contain `hub_token.txt`) |

For DSH, use `integrations/dsh/` (profile patch or local bundle). For Claude
Desktop / Cursor, copy the JSON from `integrations/claude/mcp.json` or
`integrations/cursor/mcp.json` and replace `<REPO_ROOT>`.

Example:

```json
{
  "mcpServers": {
    "creohub": {
      "command": "cmd",
      "args": ["/c", "node", "<REPO_ROOT>\\bridge\\bridge.mjs"],
      "env": { "CREO_HUB_ROOT": "<REPO_ROOT>" }
    }
  }
}
```

## Tool surface

| Tool | Kind | Purpose |
|---|---|---|
| `creo_hub_status` | read | Port discovery, version, features, executor, queue, token validity |
| `creo_hub_plugins` | read | Live plugin state cards (state, fingerprint, autoMount, lastErr) |
| `creo_hub_manifest` | read | Full manifest: commands, risk, `descForLLM`, `inputSchema` |
| `creo_hub_log` | read | Incremental audit log tail (`offset` in bytes) |
| `creo_hub_mount` `{id}` | mutate | Mount a plugin (idempotent when ACTIVE) |
| `creo_hub_unmount` `{id}` | mutate | Unmount; `TAINTED` means restart Creo, do not retry |
| `creo_hub_call` `{plugin, command, args, dryRun?}` | mutate/read | Execute a declared command; confirm commands need panel approval |
| `creo_hub_job` `{jobId?}` | read | Poll async jobs (`queued/working/completed/failed/cancelled`) |

Every declared plugin command is also exposed as a first-class tool
(`<plugin>__<command>`); the list updates at runtime (`tools/list_changed`).
Hosts that cache tools may need a refresh.

## Error codes

| `err_code` | Meaning → action |
|---|---|
| `HUB_UNREACHABLE` | No hub on 8765–8775 → start Creo / verify the hub is running on the same machine |
| `TOKEN_MISSING` | `<root>\hub_token.txt` missing/invalid → fix `CREO_HUB_ROOT` |
| `TOKEN_STALE` | Token rejected → root points at a different installation |
| `BUSY_TIMEOUT` | Creo main thread busy → inspect audit/log, retry later |
| `HUB_NOT_READY` / `HUB_STOPPING` | Creo starting/stopping → wait and retry |
| `CONTRACT_MISMATCH` | Non-JSON/404 → port occupied by another program, or hub too old |

## Security invariants

- stdio only; no listening socket.
- Token is read per call and never logged or returned.
- stdout carries JSON-RPC frames only (diagnostics go to stderr).
- Audit is authoritative on the hub side; the bridge keeps no state.

## Offline / air-gapped

The bridge needs Node.js and the bundled `node_modules` (one SDK). On an
air-gapped machine, run `npm install` on a staging machine with the same OS and
ship the directory, then point `command` at the absolute `node.exe`.
