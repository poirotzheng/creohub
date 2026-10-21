# Integrations

creo-hub speaks **MCP over stdio**, so it can be attached to any compatible host.
This directory contains copy-paste configurations.

| Host | File | Notes |
|---|---|---|
| DSH (DeepSeek Harness) | [`dsh/`](dsh/) | Profile patch or local bundle; tools appear as `mcp__creohub__*` |
| Claude Desktop | [`claude/mcp.json`](claude/mcp.json) | Merge into your client config; Windows uses `cmd /c` |
| Cursor | [`cursor/mcp.json`](cursor/mcp.json) | Same shape as Claude Desktop |

Replace `<REPO_ROOT>` with the absolute path of your checkout. The bridge reads
the hub token from `<REPO_ROOT>\hub_token.txt` on every call; Creo and the hub
must be running.

See the main [README](../README.md) and [bridge README](../bridge/README.md).
