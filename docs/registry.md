# OS Registry

The registry is the OS source of truth for **what is installed, how it is configured, and
which app opens which file**. It lives on SD under `/system/registry/` and is exposed to
the web shell over the event protocol (`registry.*` topics).

## Files

| File | Purpose |
|---|---|
| `apps.json` | Index of installed apps: id, version, path, enabled, autostart, granted permissions |
| `file-associations.json` | Extension → default app id, plus a fallback handler |
| `settings.json` | System settings: device, network, power, UI, security |

## How file opening works

1. User opens a file (from File Commander, or a `fs.open` event).
2. The OS reads its extension and looks it up in `file-associations.json` → `default_open`.
3. If found, it launches that app with the file path; the app must declare the extension
   in its manifest `handles.extensions` (the OS verifies this).
4. If not found, it falls back to `fallback` (File Commander, which offers "Open with…").

Example: `notes.txt` → `default_open.txt = "notepad"` → Notepad opens it.
`photo.jpg` → `default_open.jpg = "photo-viewer"` → Photo Viewer opens it.

## Lifecycle events

| Topic | When |
|---|---|
| `registry.app_installed` | A bundle is verified and added to `apps.json` |
| `registry.app_removed` | A bundle is uninstalled |
| `registry.association_changed` | A default handler changes |
| `registry.settings_changed` | A setting is updated |

## Compatibility checks

When scanning `/apps`, the loader skips an app whose manifest `platforms` does not include
the running SoC, or whose `min_os` is newer than the running OS. This keeps an SD card
moved from another device (or kept across an OS upgrade) from loading an incompatible app.
See [app-runtimes.md](app-runtimes.md).

## Editing rules

- The registry is written only by the OS (atomic write + journal entry), never by apps
  directly — apps request changes via events, which the OS validates against permissions.
- Every write appends an event to `/journal`, so registry state is replayable and undoable.
