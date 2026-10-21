# Security policy

## Supported versions

| Version | Supported |
|---|---|
| 1.x | ✅ |

## Reporting a vulnerability

Please **do not** open a public issue. Use one of:

- GitHub Security Advisories (preferred, once the public repository exists);
- email `creohubsecurity@163.com` with subject `[creo-hub security]`.

Include: affected version, Windows/Creo version, reproduction steps, and impact.
We aim to acknowledge within 7 days and to publish a fix or mitigation within
90 days.

## Security model (what to expect)

- The hub HTTP API binds to **127.0.0.1 only**; every request requires a bearer
  token read from `hub_token.txt`. Origin/Host checks reject cross-site calls.
- Mutating commands are gated by `risk` (`confirm`/`destructive` require a
  two-step nonce approval in the panel). `safe` commands are read-only by
  contract.
- The hub DLL never calls a network service other than its own loopback panel;
  LLM API keys live only in the bridge/agent configuration, never in the DLL.
- Audit lines are append-only in `logs/hub*.log`; every mount/unmount/call is
  recorded.
- The panel is a single static HTML file with no external resources/CDNs.

## Out of scope

- Vulnerabilities requiring a compromised local user account with access to
  `hub_token.txt`.
- Issues caused by running Creo/TOOLKIT without a valid license.
- Third-party plugin code (report to the plugin author).

## Hardening tips

- Keep `hub_token.txt` out of version control and backups you share.
- Run Creo under a dedicated Windows account for automation.
- Prefer `safe` commands in unattended scripts; gate `confirm` commands behind a
  human.
