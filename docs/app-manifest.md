# App Manifest Schema

Every app is a bundle on SD with its own `manifest.json`. Keep one app per folder.

```
/apps/<id>/
  manifest.json     # this schema
  manifest.sig      # Ed25519 signature of manifest + content hashes
  www/              # web UI (served under web_route)
  data/             # app-private storage
  assets/           # icons, static assets
  rules/            # optional automation bytecode
```

## Fields

| Field | Type | Meaning |
|---|---|---|
| `id` | string | Unique, kebab-case. Matches folder name |
| `name` | string | Display name |
| `version` | semver | Display version (e.g. `1.2.0`) |
| `version_code` | int | Technical version, incremented every release â€” drives updates |
| `min_os` | semver | Minimum NucleoOS version required (compatibility gate) |
| `platforms` | string[] | Supported SoCs (e.g. `["esp32s3"]`); checked at load |
| `description` | string | One line |
| `category` | enum | `system` \| `productivity` \| `media` \| `tools` \| `connectivity` \| `games` |
| `runtime` | enum | `web` (default) \| `vm` \| `service` \| `elf` â€” see [app-runtimes.md](app-runtimes.md) |
| `entry_service` | string | Service symbol launched by the runtime |
| `web_route` | path | Where the app UI is mounted |
| `icon` | path | Icon under `assets/` |
| `permissions` | string[] | Capabilities requested (see below) |
| `mounts` | object | Logical mount â†’ SD path |
| `handles` | object | File types this app opens (see below) |
| `subscribes` | string[] | Event topics consumed |
| `publishes` | string[] | Event topics emitted |
| `power` | object | `budget_class` (`low`\|`medium`\|`high`) + `wants_wakeup[]` |
| `mesh` | object | `exposes[]` / `consumes[]` topics for the swarm |

## `handles` â€” file association source

An app advertises which file types it can open. The registry's
`file-associations.json` chooses the **default** app per extension.

```json
"handles": {
  "role": "editor",                       // editor | viewer | player | none
  "extensions": ["txt", "md", "log"]
}
```

## `intents` â€” actions an app can fulfil

Beyond opening a file by extension, an app can declare the **actions** it handles
(`view` / `edit` / `play` / `share`). The OS resolves a request like
`{action:"view", uri:"/data/x.jpg"}` to an app that declares that action for `jpg`.
This is the general dispatch layer above file associations.

```json
"intents": [ { "action": "edit", "extensions": ["txt", "md"] } ]
```

## Permissions (capabilities)

`storage.app`, `storage.shared`, `system.events`, `system.clipboard`, `system.notify`,
`device.keyboard`, `device.display`, `device.ir`, `device.mic`, `device.audio`,
`net.wifi`, `net.ble`, `mesh.peer`, `power.budget`.

The runtime enforces that an app only touches what it declares â€” automation rules included.
