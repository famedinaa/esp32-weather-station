// Wire protocol of the plant monitor (MQTT IoT Panel):
//   plant/<id>/telemetry  {"boot":1763650000000,"seq":42,"ts":"2026-11-20T15:04:05.123Z","temp":21.4,"hum":55.2,"pres":1012.8}
//   plant/<id>/status     "online" (retained); the broker publishes the last will "offline" if the link dies
//   plant/<id>/resend     {"boot":1763650000000,"from":40,"to":41}  <- the server asks for readings it missed
// `boot` is the epoch ms of this power-up and `seq` counts from 1, so (boot, seq) identifies a reading
// across restarts and the server can drop duplicates and spot gaps.
//
// Pure C++ (no Arduino), so it compiles and is unit tested on the PC.
#pragma once
#include <cstddef>
#include <cstdint>

namespace station {

struct Reading {
  uint32_t seq = 0;
  int64_t ts_ms = 0;      // Unix epoch, milliseconds
  float temp = 0;         // °C   (NaN = sensor value rejected -> JSON null)
  float hum = 0;          // %RH
  float pres = 0;         // hPa
};

struct ResendRequest {
  uint64_t boot = 0;
  uint32_t from = 0;
  uint32_t to = 0;
};

enum class TopicKind { Telemetry, Status, Resend, Other };

// "2026-11-20T15:04:05.123Z". Returns false if `cap` is too small (needs 25 bytes).
bool format_iso8601(int64_t epoch_ms, char* out, size_t cap);

// Telemetry payload. Returns the length written, or -1 if it does not fit in `cap`.
int encode_telemetry(uint64_t boot, const Reading& r, char* out, size_t cap);

// Parses {"boot":…,"from":…,"to":…} in any key order, with whitespace. Rejects missing keys,
// negative or non-numeric values, and from > to.
bool parse_resend(const char* payload, size_t len, ResendRequest& out);

// "plant/<id>/<suffix>". Returns false if it does not fit.
bool make_topic(const char* device_id, const char* suffix, char* out, size_t cap);

// Classifies an incoming topic for this device.
TopicKind classify_topic(const char* device_id, const char* topic);

}  // namespace station
