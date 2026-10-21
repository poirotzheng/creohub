# DSH integration

[DeepSeek Harness (DSH)](https://github.com/deepseek-ai/deepseek-harness) is an
open-source agent harness ("Everything is a Plugin"). creo-hub integrates as a
standard **stdio MCP server**, so it also works with Claude Desktop, Cursor,
VS Code, and any other MCP host.

## Option A — profile patch (recommended)

1. Build/obtain the repository, then note its absolute path (referred to below
   as `<REPO_ROOT>`).
2. Open (or create) `%USERPROFILE%\.dsh\cordis.patch.yml`.
3. Append the entry from [`cordis.patch.yml`](cordis.patch.yml), replacing
   `<REPO_ROOT>` with your path (use forward slashes or double backslashes in
   YAML).
4. Restart DSH. The bridge tools appear as `mcp__creohub__*`.

The patch shape matters: a bare `id:` **modifies** an existing entry; new entries
must be added through `- insert:`.

## Option B — local bundle (advanced)

`bundle/` contains a minimal DSH bundle (`dsh.bundle.patch`) that inserts the
same MCP client entry. Install from a local path:

```
install_bundle <path-to>/integrations/dsh/bundle
```

No npm publication is required.

## Tool surface

Management tools: `creo_hub_status`, `creo_hub_plugins`, `creo_hub_manifest`,
`creo_hub_log`, `creo_hub_mount`, `creo_hub_unmount`, `creo_hub_call`,
`creo_hub_job`.

Every plugin command declared in `plugin.json` is also exposed as a first-class
tool (`<plugin>__<command>`), and the list updates at runtime
(`tools/list_changed`). Hosts that cache tool lists may need a refresh.

## Verify

```bat
curl http://127.0.0.1:8765/api/v1/health
```

Then ask the host: "list creo-hub plugins". The answer must match the panel.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `HUB_UNREACHABLE` | Creo/hub not running, or wrong root | Start Creo; check `CREO_HUB_ROOT` |
| `TOKEN_*` | `hub_token.txt` missing/changed | Restart Creo (token regenerates) or fix root |
| Tools missing | DSH profile not applied / not restarted | Re-check `cordis.patch.yml`, restart |
| 428 on a command | `confirm`/`destructive` needs human approval | Approve in the Creo hub panel, retry |
| Tool list stale | Host cached the list | Refresh tools / restart host |

## Security notes

- The bridge is stateless and reads the token per call from
  `<root>\hub_token.txt`.
- DSH's own approval/audit pipeline applies on top of the hub's risk gate.
- Do not run the bridge as a network service; it is stdio-only.
