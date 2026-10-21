# DSH bundle (optional)

This folder is a minimal DSH bundle that inserts the creo-hub MCP client entry
into a DSH profile. It is **optional** — the profile patch in the parent folder
is enough for most users.

```
install_bundle <path-to>/integrations/dsh/bundle
```

Before installing, replace `<REPO_ROOT>` in `dsh.bundle.patch.yml` with the
absolute path of your checkout. No npm publication is required.
