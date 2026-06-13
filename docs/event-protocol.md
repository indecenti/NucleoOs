# Event Protocol

One protocol over every transport (Wi-Fi WebSocket, BLE GATT, WebUSB, Web Serial) and
across ESP-NOW swarm peers. The event bus is append-only and **event-sourced**: every
state change is an event with a monotonic sequence number, persisted to `/journal` on SD.

## Event envelope

Compact JSON by default; CBOR is an optional binary encoding for BLE/ESP-NOW.

```json
{ "t": "wifi.status", "seq": 10472, "ts": 1717245000, "src": "nucleo-01", "d": { "state": "connected", "rssi": -58 } }
```

| Field | Meaning |
|---|---|
| `t` | Topic, dot-namespaced (`system.*`, `wifi.*`, `storage.*`, `app.<id>.*`, `mesh.*`) |
| `seq` | Monotonic u32 per device; gaps mean dropped/needs-resync |
| `ts` | Unix seconds |
| `src` | Origin device id (set for swarm events) |
| `d` | Topic-specific payload |

## Delta sync (the RAM-saving core)

The client never receives full state. On connect it sends the last `seq` it holds; the
device streams only events with `seq > last`. A snapshot is offered only on first pairing
or after a gap too large to replay.

```
client → { "op": "subscribe", "topics": ["wifi.*","storage.*"], "since": 10470 }
device → events 10471, 10472, ... (live thereafter)
```

Commands are events too:

```
client → { "op": "publish", "t": "fs.delete", "d": { "path": "/data/old.txt" } }
device → { "t": "fs.changed", "seq": 10480, "d": { "op": "delete", "path": "/data/old.txt" } }
```

## Framing per transport

- **WebSocket:** one JSON text frame per event.
- **BLE GATT:** notify on a TX characteristic; length-prefixed chunks reassembled to fit
  the negotiated MTU (~244 B). A separate RX characteristic carries client commands.
- **WebUSB / Web Serial:** newline-delimited JSON (NDJSON) over CDC.
- **ESP-NOW (swarm):** CBOR envelope in a single 250-byte ESP-NOW frame; larger payloads
  are chunked with `seq`+`part` fields.

## Swarm extension

Events with topic `mesh.*` or marked `"src"` other than self are gossiped to peers the app
declared in its manifest `mesh.consumes`. Remote events arrive on the local bus exactly
like local ones, so apps need no special code to be swarm-aware.
