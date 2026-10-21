# Contributing to creo-hub

Thanks for your interest. This project manages Creo Parametric plugins; it is
developed on Windows with Visual Studio and the PTC Pro/TOOLKIT SDK.

## Prerequisites

- Windows 10 x64 (Windows 7 profile exists but is unverified).
- Visual Studio 2017 or 2022 with C++ desktop workload.
- Creo Parametric 6.0 with a valid TOOLKIT license option.
- The Pro/TOOLKIT SDK headers/import libraries (from your Creo installation).
- Node.js 18+ (for the bridge, agent, tooling and most gates).
- Inno Setup 6.3.3 (only if you build the installer).

Set the toolkit root once:

```bat
deploy\configure.cmd
```

or create `local.env` from `local.env.example`. `local.env` is ignored by git.

## Build and test

```bat
build\build.cmd
test\run_all.cmd
```

`test\GATES.md` lists every gate, its preconditions, and whether it runs in
public CI. Gates that need Creo or the PTC SDK are local-only.

## Pull requests

1. Open an issue first for non-trivial changes.
2. Keep changes focused; do not reformat unrelated files.
3. Add or update tests/gates for behavior changes.
4. Run `test\run_all.cmd` and paste the summary in the PR.
5. Sign the CLA (see `CLA.md`) — required for external contributions.

## Commit style

Use short imperative subjects, optional scopes:

```
feat(panel): add job progress badge
fix(deploy): resolve toolkit root from local.env
docs(readme): clarify protk_unlock requirement
```

## Code conventions

- C: C89-compatible style used by the existing hub (`/MT`, Win7 discipline);
  do not introduce C++ or new runtime dependencies without discussion.
- PowerShell/batch: keep `.cmd` files CRLF + ASCII; avoid parentheses inside
  `if (...)` blocks that expand paths (see existing scripts).
- Panel: ES5 only, no external assets/CDNs.
- Node: zero-dependency single files where possible; no telemetry.

## Security

Do not open public issues for vulnerabilities. See `SECURITY.md`.

## License

By contributing you agree that your contribution is licensed under the
Apache License 2.0 and to the terms of `CLA.md`.
