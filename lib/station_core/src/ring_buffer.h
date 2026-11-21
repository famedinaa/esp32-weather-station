// Fixed-capacity history of readings, indexed by sequence number. No heap: it lives in a static array,
// which on the ESP32 means the size is known at link time and it can't fragment memory after weeks up.
// When full, the oldest reading is overwritten.
#pragma once
#include <cstddef>
#include <cstdint>

#include "protocol.h"

namespace station {

template <size_t N>
class ReadingRing {
  static_assert(N > 0, "capacity must be positive");

 public:
  // Readings must arrive with consecutive sequence numbers (the station assigns them).
  void push(const Reading& r) {
    slots_[r.seq % N] = r;
    if (count_ < N) ++count_;
    newest_ = r.seq;
  }

  // nullptr if `seq` was never stored or has already been overwritten.
  const Reading* find(uint32_t seq) const {
    if (count_ == 0 || seq > newest_ || seq < oldest_seq()) return nullptr;
    const Reading& r = slots_[seq % N];
    return r.seq == seq ? &r : nullptr;
  }

  uint32_t oldest_seq() const { return count_ == 0 ? 0 : newest_ - static_cast<uint32_t>(count_) + 1; }
  uint32_t newest_seq() const { return newest_; }
  size_t size() const { return count_; }
  static constexpr size_t capacity() { return N; }

 private:
  Reading slots_[N]{};
  size_t count_ = 0;
  uint32_t newest_ = 0;
};

}  // namespace station
