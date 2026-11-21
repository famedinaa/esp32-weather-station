// Sensor-side logic that does not depend on the hardware: plausibility limits, spike rejection,
// reconnect backoff and the simulated sensor used when no BME280 answers on I2C.
#pragma once
#include <cmath>
#include <cstdint>

namespace station {

constexpr double kPi = 3.14159265358979323846;

// BME280 datasheet operating ranges. Outside them the value is a wiring/bus problem, not weather.
struct Limits {
  float lo, hi;
  bool ok(float v) const { return std::isfinite(v) && v >= lo && v <= hi; }
};
constexpr Limits kTempLimits{-40.0f, 85.0f};
constexpr Limits kHumLimits{0.0f, 100.0f};
constexpr Limits kPresLimits{300.0f, 1100.0f};

// Median of the last three values: removes a single spike (a bad I2C read) without lagging a real change
// by more than one sample. Values that fail the limits become NaN and are published as null.
class SpikeFilter {
 public:
  explicit SpikeFilter(Limits lim) : lim_(lim) {}
  float update(float v) {
    if (!lim_.ok(v)) return NAN;
    h_[i_ % 3] = v;
    ++i_;
    if (i_ < 3) return v;
    const float a = h_[0], b = h_[1], c = h_[2];
    return std::fmax(std::fmin(a, b), std::fmin(std::fmax(a, b), c));
  }

 private:
  Limits lim_;
  float h_[3] = {0, 0, 0};
  uint32_t i_ = 0;
};

// Exponential backoff for Wi-Fi/MQTT reconnects: 1 s, 2 s, 4 s … capped at 60 s, with ±20 % jitter so
// a building full of devices doesn't hammer the broker in lockstep after a power cut.
class Backoff {
 public:
  Backoff(uint32_t base_ms = 1000, uint32_t max_ms = 60000) : base_(base_ms), max_(max_ms), cur_(base_ms) {}
  // `rnd` in [0, 1).
  uint32_t next(float rnd) {
    const uint32_t d = cur_;
    cur_ = cur_ >= max_ / 2 ? max_ : cur_ * 2;
    const float j = 0.8f + 0.4f * rnd;
    return static_cast<uint32_t>(static_cast<float>(d) * j);
  }
  void reset() { cur_ = base_; }

 private:
  uint32_t base_, max_, cur_;
};

// Deterministic stand-in for the BME280: a slow daily cycle plus noise, so the firmware (and the host
// build) produce believable data with no sensor attached.
class SimulatedSensor {
 public:
  explicit SimulatedSensor(uint32_t seed = 12345) : s_(seed) {}
  void read(int64_t epoch_ms, float& temp, float& hum, float& pres) {
    const double day = 86400000.0;
    const double phase = std::fmod(static_cast<double>(epoch_ms), day) / day * 2 * kPi;
    temp = static_cast<float>(18.0 + 6.0 * std::sin(phase - kPi / 2) + noise() * 0.15);
    hum = static_cast<float>(60.0 - 15.0 * std::sin(phase - kPi / 2) + noise() * 0.8);
    pres = static_cast<float>(1013.0 + 2.0 * std::sin(phase / 2) + noise() * 0.1);
  }

 private:
  double noise() {
    s_ = s_ * 1664525u + 1013904223u;
    return (static_cast<double>(s_ >> 8) / 16777216.0) * 2.0 - 1.0;
  }
  uint32_t s_;
};

}  // namespace station
