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

// Called for every published event (e.g. the WebSocket layer broadcasts it live).
typedef void (*nucleo_event_sink_t)(uint32_t seq, const char *topic, const char *payload_json);

// Open the journal and reset state.
esp_err_t nucleo_event_init(void);

// Publish an event. payload_json must be a JSON object string (or "{}"). Returns its seq.
uint32_t nucleo_event_publish(const char *topic, const char *payload_json);

// Highest assigned sequence number.
uint32_t nucleo_event_current_seq(void);

// Register the live sink (one consumer; typically the WebSocket broadcaster).
void nucleo_event_set_sink(nucleo_event_sink_t sink);

// Serialize buffered events with seq > since into out as a JSON array. Returns count,
// or -1 if `since` is older than the ring window (client should resync).
int nucleo_event_copy_since(uint32_t since, char *out, size_t out_sz);
