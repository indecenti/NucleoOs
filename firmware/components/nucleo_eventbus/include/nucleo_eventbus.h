// Event-sourced bus: monotonic events, journaled to SD, with a recent-events ring
// for WebSocket delta replay. See docs/event-protocol.md.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Capacity (incl. NUL) of one event's topic / payload as stored in the replay ring. Public so
// publishers can bound their JSON to fit a slot — anything longer is truncated when buffered, so a
// publisher that might exceed PAYLOAD_MAX must clip its variable part itself and keep the JSON VALID.
#define NUCLEO_EVENT_TOPIC_MAX   48
#define NUCLEO_EVENT_PAYLOAD_MAX 208

// Called for every published event. `src` is NULL for locally-published events and the origin
// device id for events that arrived from a peer via nucleo_event_inject() (lets a mesh sink avoid
// re-forwarding foreign events — structural loop prevention).
typedef void (*nucleo_event_sink_t)(uint32_t seq, const char *topic, const char *payload_json,
                                    const char *src);

// Open the journal and reset state.
esp_err_t nucleo_event_init(void);

// Publish a locally-originated event. payload_json must be a JSON object string (or "{}"). Returns its seq.
uint32_t nucleo_event_publish(const char *topic, const char *payload_json);

// Inject an event that originated on a peer (received over the mesh) onto the LOCAL bus: it gets a
// fresh local seq so the WebSocket client can order/replay it, and is delivered to every sink with
// src=<origin id>. De-duplication by (origin id, origin seq) is the caller's job (the mesh layer),
// done BEFORE this call — the bus stays dumb. Returns the assigned local seq.
uint32_t nucleo_event_inject(const char *src, const char *topic, const char *payload_json);

// Highest assigned sequence number.
uint32_t nucleo_event_current_seq(void);

// Register the primary live sink (slot 0; typically the WebSocket broadcaster). Back-compat.
void nucleo_event_set_sink(nucleo_event_sink_t sink);

// Register an additional sink (e.g. the mesh gossiper) alongside the WebSocket one. Bounded,
// no allocation. Sinks fire outside the bus lock on a stack copy, in registration order.
void nucleo_event_add_sink(nucleo_event_sink_t sink);

// Serialize buffered events with seq > since into out as a JSON array. Returns count,
// or -1 if `since` is older than the ring window (client should resync).
int nucleo_event_copy_since(uint32_t since, char *out, size_t out_sz);
