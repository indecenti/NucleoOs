# Storage & SD Filesystem

The microSD is the OS working filesystem; the firmware lives in flash. This doc defines
how the card is mounted, detected on first boot, and kept healthy.

## Mount model

- Firmware (kernel + core services) runs from flash (OTA slots). Immutable at runtime.
- The SD card is mounted via **ESP-IDF FATFS** over **SPI3** — its own SPI host, separate
  from the display (SPI2) — so there is no shared-bus contention. It holds all mutable
  content: apps, data, config, logs, journal, assets.
- Supported filesystems: **FAT32** and **exFAT** (a 64 GB card ships exFAT). exFAT must be
  enabled in the IDF FATFS config; otherwise reformat the card to FAT32.

## Capacity & free space

On mount the OS reads volume geometry with FATFS `f_getfree()` and computes:

```
total_bytes = (n_clusters)        * cluster_size
free_bytes  = (n_free_clusters)   * cluster_size
used_bytes  = total_bytes - free_bytes
```

These are cached in RAM, refreshed on every `fs.changed` event, and exposed as
`storage.status` (see below). The Dashboard, Settings and File Commander all show them.

## First-boot provisioning

The first time a card is seen (no `/system/volume.json` present), the OS provisions it:

1. **Detect card** — present? readable? If absent → recovery screen + `storage.missing`.
2. **Identify filesystem** — FAT32/exFAT ok; unformatted or unsupported → offer format
   (FAT32 ≤ 32 GB, exFAT otherwise), guarded by an explicit user confirmation.
3. **Read capacity** — `total_bytes` / `free_bytes` via `f_getfree()`.
4. **Bootstrap the system tree** if missing:
   `/system/{config,registry,sessions,keys,logs}`, `/journal`, `/apps`,
   `/data/{shared,imports,exports}`, `/data/{Pictures,Music,Videos}`, `/backups`, `/www/shell`.
5. **Write the volume record** `/system/volume.json` (see schema below).
6. **Emit events** `storage.provisioned` then `storage.mounted` with the volume record.

On later boots, step 4 is a quick "ensure tree exists" check, then straight to mounted.

## Volume record (`/system/volume.json`)

```json
{
  "fs": "exfat",
  "label": "NUCLEO",
  "total_bytes": 63864569856,
  "cluster_size": 131072,
  "provisioned_at": 1717245000,
  "os_version": "0.1.0",
  "device_id": "nucleo-01"
}
```

Live free space is **not** stored here (it changes constantly); it travels in
`storage.status` events instead.

## Events

| Topic | Payload | Meaning |
|---|---|---|
| `storage.missing` | `{}` | No usable card |
| `storage.mounted` | volume record + `free_bytes` | Card mounted and ready |
| `storage.provisioned` | `{ created: [paths] }` | First-boot tree created |
| `storage.status` | `{ total_bytes, free_bytes, used_bytes }` | Capacity snapshot |
| `storage.changed` | `{ op, path }` | A file/dir changed (triggers status refresh) |
| `storage.ejected` | `{}` | Hot-swap removal detected |

## Health & safety

- **Hot-swap:** removal mid-operation is detected; pending writes fail safely and apps get
  `storage.ejected`. The OS itself keeps running from flash.
- **Atomic writes:** system files (registry, volume record) use write-temp-then-rename plus
  a journal entry, so a power loss never leaves a half-written registry.
- **Low space:** below a threshold the OS emits a warning and blocks non-essential writes
  (logs rotate, caches purge) before the card fills.
- **Corruption:** if the system tree is unreadable, boot continues from flash into recovery,
  where File Commander / Settings can reformat or restore from `/backups`.
