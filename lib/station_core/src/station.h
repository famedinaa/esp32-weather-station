// The station's behaviour, independent of Wi-Fi, MQTT library and sensor:
//  - numbers every reading (boot, seq) and keeps the last kHistory in a ring;
//  - publishes live while connected; while offline it keeps sampling and, on reconnect, sends what the
//    broker never saw, oldest first, a few per loop so it doesn't starve the rest of the firmware;
//  - answers resend requests from the server (a message published QoS 0 can be lost by the broker after
//    the device handed it over; the server sees the gap in seq and asks again).
// The firmware (src/main.cpp) and the PC build (host/) both drive this same class.
#pragma once
#include <cstddef>
#include <cstdint>

#include "protocol.h"
#include "ring_buffer.h"

namespace station {

constexpr size_t kHistory = 512;        // ~17 min at one reading every 2 s; 512 * 24 B = 12 KB of RAM
constexpr size_t kMaxResendRanges = 8;

class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool connected() const = 0;
  // false = not handed to the network stack (link down, buffer full): the station retries later.
  virtual bool publish(const char* topic, const char* payload, bool retain) = 0;
};

struct Stats {
  uint32_t sampled = 0;
  uint32_t published_live = 0;
  uint32_t published_late = 0;     // sent after reconnecting (store and forward)
  uint32_t resent = 0;             // sent again on the server's request
  uint32_t lost_overwritten = 0;   // never sent: offline longer than the history
  uint32_t resend_unavailable = 0; // requested but no longer in the history
  uint32_t resend_ignored = 0;     // malformed, or for another boot
  uint32_t publish_failures = 0;
};

class Station {
 public:
  Station(const char* device_id, Transport& transport, uint16_t max_sends_per_loop = 20);

  void begin(uint64_t boot_epoch_ms);
  uint64_t boot() const { return boot_; }

  // A new sample: assigned the next seq, stored, and published if the link is up and nothing older is queued.
  void on_sample(int64_t ts_ms, float temp, float hum, float pres);
  // Call right after (re)connecting and subscribing: announces "online" and starts draining the backlog.
  void on_connected();
  void on_message(const char* topic, const uint8_t* payload, size_t len);
  // Call every loop: drains the backlog and pending resends within the per-loop budget.
  void loop();

  uint32_t last_seq() const { return seq_; }
  uint32_t backlog() const { return seq_ >= next_unsent_ ? seq_ - next_unsent_ + 1 : 0; }
  const Stats& stats() const { return stats_; }

 private:
  bool send(const Reading& r);

  const char* id_;
  Transport& tx_;
  uint16_t budget_;
  uint64_t boot_ = 0;
  uint32_t seq_ = 0;
  uint32_t next_unsent_ = 1;     // first seq the broker has not been handed yet
  ReadingRing<kHistory> ring_;
  ResendRequest resends_[kMaxResendRanges]{};
  size_t n_resends_ = 0;
  char topic_telemetry_[64]{};
  char topic_status_[64]{};
  Stats stats_;
};

}  // namespace station
