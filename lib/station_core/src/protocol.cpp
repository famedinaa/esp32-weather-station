#include "protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace station {

namespace {

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm, valid for the proleptic Gregorian calendar).
void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yy + (m <= 2));
}

// Appends a number or `null` (NaN/inf readings are rejected values, not zeros).
int put_value(char* out, size_t cap, const char* key, float v, int decimals) {
  if (!std::isfinite(v)) return std::snprintf(out, cap, ",\"%s\":null", key);
  return std::snprintf(out, cap, ",\"%s\":%.*f", key, decimals, static_cast<double>(v));
}

const char* skip_ws(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  return p;
}

// Finds "key" followed by ':' and an unsigned integer. Strict: digits only, no sign, no fraction.
bool find_uint(const char* s, size_t len, const char* key, uint64_t& value) {
  const char* end = s + len;
  const size_t klen = std::strlen(key);
  for (const char* p = s; p + klen + 2 <= end; ++p) {
    if (*p != '"' || std::memcmp(p + 1, key, klen) != 0 || p[klen + 1] != '"') continue;
    const char* q = skip_ws(p + klen + 2, end);
    if (q >= end || *q != ':') return false;
    q = skip_ws(q + 1, end);
    if (q >= end || *q < '0' || *q > '9') return false;
    uint64_t v = 0;
    int digits = 0;
    while (q < end && *q >= '0' && *q <= '9') {
      if (++digits > 16) return false;  // > 2^53: not a safe JSON integer
      v = v * 10 + static_cast<uint64_t>(*q - '0');
      ++q;
    }
    q = skip_ws(q, end);
    if (q < end && *q != ',' && *q != '}') return false;  // 12.5, 12abc, …
    value = v;
    return true;
  }
  return false;
}

}  // namespace

bool format_iso8601(int64_t epoch_ms, char* out, size_t cap) {
  if (cap < 25) return false;
  int64_t secs = epoch_ms >= 0 ? epoch_ms / 1000 : (epoch_ms - 999) / 1000;
  const int ms = static_cast<int>(epoch_ms - secs * 1000);
  int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
  const int64_t sod = secs - days * 86400;
  int y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  const int n = std::snprintf(out, cap, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y, m, d, static_cast<int>(sod / 3600),
                              static_cast<int>(sod % 3600 / 60), static_cast<int>(sod % 60), ms);
  return n > 0 && static_cast<size_t>(n) < cap;
}

int encode_telemetry(uint64_t boot, const Reading& r, char* out, size_t cap) {
  char ts[32];
  if (!format_iso8601(r.ts_ms, ts, sizeof ts)) return -1;
  size_t used = 0;
  auto add = [&](int n) {
    if (n < 0 || used + static_cast<size_t>(n) >= cap) return false;
    used += static_cast<size_t>(n);
    return true;
  };
  if (!add(std::snprintf(out, cap, "{\"boot\":%llu,\"seq\":%lu,\"ts\":\"%s\"", static_cast<unsigned long long>(boot),
                         static_cast<unsigned long>(r.seq), ts)))
    return -1;
  if (!add(put_value(out + used, cap - used, "temp", r.temp, 2))) return -1;
  if (!add(put_value(out + used, cap - used, "hum", r.hum, 1))) return -1;
  if (!add(put_value(out + used, cap - used, "pres", r.pres, 1))) return -1;
  if (!add(std::snprintf(out + used, cap - used, "}"))) return -1;
  return static_cast<int>(used);
}

bool parse_resend(const char* payload, size_t len, ResendRequest& out) {
  if (payload == nullptr) return false;
  uint64_t boot, from, to;
  if (!find_uint(payload, len, "boot", boot) || !find_uint(payload, len, "from", from) ||
      !find_uint(payload, len, "to", to))
    return false;
  if (from == 0 || from > to || to > UINT32_MAX) return false;
  out.boot = boot;
  out.from = static_cast<uint32_t>(from);
  out.to = static_cast<uint32_t>(to);
  return true;
}

bool make_topic(const char* device_id, const char* suffix, char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "plant/%s/%s", device_id, suffix);
  return n > 0 && static_cast<size_t>(n) < cap;
}

TopicKind classify_topic(const char* device_id, const char* topic) {
  const size_t idlen = std::strlen(device_id);
  if (std::strncmp(topic, "plant/", 6) != 0 || std::strncmp(topic + 6, device_id, idlen) != 0 ||
      topic[6 + idlen] != '/')
    return TopicKind::Other;
  const char* rest = topic + 7 + idlen;
  if (std::strcmp(rest, "telemetry") == 0) return TopicKind::Telemetry;
  if (std::strcmp(rest, "status") == 0) return TopicKind::Status;
  if (std::strcmp(rest, "resend") == 0) return TopicKind::Resend;
  return TopicKind::Other;
}

}  // namespace station
