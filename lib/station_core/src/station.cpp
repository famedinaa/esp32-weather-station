#include "station.h"

namespace station {

Station::Station(const char* device_id, Transport& transport, uint16_t max_sends_per_loop)
    : id_(device_id), tx_(transport), budget_(max_sends_per_loop) {
  make_topic(id_, "telemetry", topic_telemetry_, sizeof topic_telemetry_);
  make_topic(id_, "status", topic_status_, sizeof topic_status_);
}

void Station::begin(uint64_t boot_epoch_ms) {
  boot_ = boot_epoch_ms;
  seq_ = 0;
  next_unsent_ = 1;
  n_resends_ = 0;
  ring_ = ReadingRing<kHistory>{};
  stats_ = Stats{};
}

bool Station::send(const Reading& r) {
  char payload[192];
  const int n = encode_telemetry(boot_, r, payload, sizeof payload);
  if (n < 0) return false;
  if (!tx_.publish(topic_telemetry_, payload, false)) {
    ++stats_.publish_failures;
    return false;
  }
  return true;
}

void Station::on_sample(int64_t ts_ms, float temp, float hum, float pres) {
  Reading r;
  r.seq = ++seq_;
  r.ts_ms = ts_ms;
  r.temp = temp;
  r.hum = hum;
  r.pres = pres;
  ring_.push(r);
  ++stats_.sampled;
  // Live only if nothing older is still queued: the server sees seq in order whenever possible.
  if (next_unsent_ == r.seq && tx_.connected() && send(r)) {
    ++next_unsent_;
    ++stats_.published_live;
  }
}

void Station::on_connected() { tx_.publish(topic_status_, "online", true); }

void Station::on_message(const char* topic, const uint8_t* payload, size_t len) {
  if (classify_topic(id_, topic) != TopicKind::Resend) return;
  ResendRequest req;
  if (!parse_resend(reinterpret_cast<const char*>(payload), len, req) || req.boot != boot_) {
    ++stats_.resend_ignored;
    return;
  }
  if (req.from > seq_) {  // asks for the future: nonsense
    ++stats_.resend_ignored;
    return;
  }
  if (req.to > seq_) req.to = seq_;
  // Merge with a queued range for the same boot if they overlap or touch; else queue it.
  for (size_t i = 0; i < n_resends_; ++i) {
    ResendRequest& q = resends_[i];
    if (req.from <= q.to + 1 && q.from <= req.to + 1) {
      if (req.from < q.from) q.from = req.from;
      if (req.to > q.to) q.to = req.to;
      return;
    }
  }
  if (n_resends_ < kMaxResendRanges) resends_[n_resends_++] = req;
  else ++stats_.resend_ignored;  // the server repeats requests every 2 s; it will come back
}

void Station::loop() {
  if (!tx_.connected()) return;
  uint16_t left = budget_;

  // 1. Backlog from an outage, oldest first.
  while (left > 0 && next_unsent_ <= seq_) {
    const uint32_t oldest = ring_.oldest_seq();
    if (next_unsent_ < oldest) {  // offline longer than the history: those readings are gone
      stats_.lost_overwritten += oldest - next_unsent_;
      next_unsent_ = oldest;
    }
    const Reading* r = ring_.find(next_unsent_);
    if (r == nullptr || !send(*r)) return;
    ++next_unsent_;
    ++stats_.published_late;
    --left;
  }

  // 2. Resend requests, only for readings already handed over once.
  while (left > 0 && n_resends_ > 0) {
    ResendRequest& q = resends_[0];
    if (q.from >= next_unsent_) {  // not sent yet: the backlog covers it
      q = resends_[--n_resends_];
      continue;
    }
    const Reading* r = ring_.find(q.from);
    if (r == nullptr) {
      ++stats_.resend_unavailable;
    } else if (!send(*r)) {
      return;
    } else {
      ++stats_.resent;
      --left;
    }
    if (q.from == q.to) {
      for (size_t i = 1; i < n_resends_; ++i) resends_[i - 1] = resends_[i];
      --n_resends_;
    } else {
      ++q.from;
    }
  }
}

}  // namespace station
