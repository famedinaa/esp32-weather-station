// Unit tests for station_core, run on the PC: `pio test -e native` (CI) or `python tools/test_native.py`.
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"
#include "ring_buffer.h"
#include "sensor_logic.h"
#include "station.h"

using namespace station;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- fake transport

struct FakeTransport : Transport {
  bool up = true;
  int fail_next = 0;  // refuse the next N publishes (TCP buffer full)
  struct Msg { std::string topic, payload; bool retain; };
  std::vector<Msg> sent;
  bool connected() const override { return up; }
  bool publish(const char* topic, const char* payload, bool retain) override {
    if (!up) return false;
    if (fail_next > 0) { --fail_next; return false; }
    sent.push_back({topic, payload, retain});
    return true;
  }
  std::vector<unsigned> seqs() const {
    std::vector<unsigned> out;
    for (auto& m : sent) {
      const char* p = std::strstr(m.payload.c_str(), "\"seq\":");
      if (p) out.push_back(static_cast<unsigned>(std::strtoul(p + 6, nullptr, 10)));
    }
    return out;
  }
};

static void resend(Station& s, const char* json) {
  s.on_message("plant/meteo-01/resend", reinterpret_cast<const uint8_t*>(json), std::strlen(json));
}

static const uint64_t BOOT = 1763650000000ULL;

// ---------------------------------------------------------------- protocol

void test_iso8601_known_instants() {
  char b[32];
  TEST_ASSERT_TRUE(format_iso8601(0, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00.000Z", b);
  TEST_ASSERT_TRUE(format_iso8601(951782400123LL, b, sizeof b));  // leap day 2000
  TEST_ASSERT_EQUAL_STRING("2000-02-29T00:00:00.123Z", b);
  TEST_ASSERT_TRUE(format_iso8601(1795176245999LL, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("2026-11-20T12:04:05.999Z", b);
  TEST_ASSERT_FALSE(format_iso8601(0, b, 24));  // no room for the terminator
}

void test_telemetry_payload() {
  Reading r;
  r.seq = 42;
  r.ts_ms = 1795176245123LL;
  r.temp = 21.437f;
  r.hum = 55.25f;
  r.pres = 1012.84f;
  char b[192];
  const int n = encode_telemetry(BOOT, r, b, sizeof b);
  TEST_ASSERT_EQUAL_STRING(
      "{\"boot\":1763650000000,\"seq\":42,\"ts\":\"2026-11-20T12:04:05.123Z\",\"temp\":21.44,\"hum\":55.2,\"pres\":1012.8}", b);
  TEST_ASSERT_EQUAL(static_cast<int>(std::strlen(b)), n);
}

void test_rejected_values_are_null_not_zero() {
  Reading r;
  r.seq = 1;
  r.temp = NAN;
  r.hum = 50;
  r.pres = INFINITY;
  char b[192];
  encode_telemetry(BOOT, r, b, sizeof b);
  TEST_ASSERT_NOT_NULL(std::strstr(b, "\"temp\":null"));
  TEST_ASSERT_NOT_NULL(std::strstr(b, "\"pres\":null"));
  TEST_ASSERT_NOT_NULL(std::strstr(b, "\"hum\":50.0"));
}

void test_payload_that_does_not_fit_fails_cleanly() {
  Reading r;
  r.seq = 1;
  char b[40];
  TEST_ASSERT_EQUAL(-1, encode_telemetry(BOOT, r, b, sizeof b));
}

void test_parse_resend_accepts_the_servers_format_in_any_order() {
  ResendRequest q;
  const char* a = "{\"boot\":1763650000000,\"from\":40,\"to\":41}";
  TEST_ASSERT_TRUE(parse_resend(a, std::strlen(a), q));
  TEST_ASSERT_EQUAL_UINT64(BOOT, q.boot);
  TEST_ASSERT_EQUAL_UINT32(40, q.from);
  TEST_ASSERT_EQUAL_UINT32(41, q.to);
  const char* b = "{ \"to\" : 9 , \"boot\":1, \"from\": 7 }";
  TEST_ASSERT_TRUE(parse_resend(b, std::strlen(b), q));
  TEST_ASSERT_EQUAL_UINT32(7, q.from);
  TEST_ASSERT_EQUAL_UINT32(9, q.to);
}

void test_parse_resend_rejects_garbage() {
  ResendRequest q;
  const char* bad[] = {
      "",
      "{}",
      "{\"boot\":1,\"from\":5}",                 // missing to
      "{\"boot\":1,\"from\":5,\"to\":4}",        // from > to
      "{\"boot\":1,\"from\":0,\"to\":4}",        // seq starts at 1
      "{\"boot\":1,\"from\":-5,\"to\":4}",
      "{\"boot\":1,\"from\":5.5,\"to\":6}",
      "{\"boot\":\"x\",\"from\":5,\"to\":6}",
      "{\"boot\":1,\"from\":5,\"to\":99999999999}",  // > uint32
      "{\"boot\":1,\"from\":5,\"to\":12345678901234567890}",
      "not json at all",
  };
  for (const char* s : bad) TEST_ASSERT_FALSE_MESSAGE(parse_resend(s, std::strlen(s), q), s);
  // Not NUL-terminated: the parser must respect `len`.
  const char trunc[] = {'{', '"', 'b', 'o', 'o', 't', '"', ':', '1'};
  TEST_ASSERT_FALSE(parse_resend(trunc, sizeof trunc, q));
}

void test_topics() {
  char t[64];
  TEST_ASSERT_TRUE(make_topic("meteo-01", "telemetry", t, sizeof t));
  TEST_ASSERT_EQUAL_STRING("plant/meteo-01/telemetry", t);
  TEST_ASSERT_FALSE(make_topic("meteo-01", "telemetry", t, 10));
  TEST_ASSERT_EQUAL(static_cast<int>(TopicKind::Resend), static_cast<int>(classify_topic("meteo-01", "plant/meteo-01/resend")));
  TEST_ASSERT_EQUAL(static_cast<int>(TopicKind::Other), static_cast<int>(classify_topic("meteo-01", "plant/meteo-012/resend")));
  TEST_ASSERT_EQUAL(static_cast<int>(TopicKind::Other), static_cast<int>(classify_topic("meteo-01", "plant/meteo-01/resend/x")));
}

// ---------------------------------------------------------------- ring

void test_ring_overwrites_oldest() {
  ReadingRing<4> ring;
  TEST_ASSERT_NULL(ring.find(1));
  for (uint32_t s = 1; s <= 6; ++s) {
    Reading r;
    r.seq = s;
    ring.push(r);
  }
  TEST_ASSERT_EQUAL(4, ring.size());
  TEST_ASSERT_EQUAL_UINT32(3, ring.oldest_seq());
  TEST_ASSERT_NULL(ring.find(2));
  TEST_ASSERT_NOT_NULL(ring.find(3));
  TEST_ASSERT_EQUAL_UINT32(6, ring.find(6)->seq);
  TEST_ASSERT_NULL(ring.find(7));
}

// ---------------------------------------------------------------- sensor logic

void test_spike_filter_removes_single_spike_and_rejects_out_of_range() {
  SpikeFilter f(kTempLimits);
  f.update(20.0f);
  f.update(20.1f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 20.1f, f.update(20.2f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 20.2f, f.update(60.0f));   // bad read: median ignores it
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 20.3f, f.update(20.3f));
  TEST_ASSERT_TRUE(std::isnan(f.update(150.0f)));           // impossible for a BME280
  TEST_ASSERT_TRUE(std::isnan(f.update(NAN)));
}

void test_backoff_doubles_to_cap_with_jitter_and_resets() {
  Backoff b(1000, 60000);
  TEST_ASSERT_EQUAL_UINT32(1000, b.next(0.5f));
  TEST_ASSERT_EQUAL_UINT32(2000, b.next(0.5f));
  TEST_ASSERT_EQUAL_UINT32(3200, b.next(0.0f));   // 4000 * 0.8
  for (int i = 0; i < 10; ++i) b.next(0.5f);
  TEST_ASSERT_EQUAL_UINT32(60000, b.next(0.5f));
  TEST_ASSERT_UINT32_WITHIN(1, 71999, b.next(0.99999f));
  b.reset();
  TEST_ASSERT_EQUAL_UINT32(1000, b.next(0.5f));
}

void test_simulated_sensor_stays_within_limits() {
  SimulatedSensor s;
  for (int64_t t = 0; t < 86400000LL * 2; t += 600000) {
    float a, h, p;
    s.read(t, a, h, p);
    TEST_ASSERT_TRUE(kTempLimits.ok(a) && kHumLimits.ok(h) && kPresLimits.ok(p));
  }
}

// ---------------------------------------------------------------- station

void test_live_publishing_in_order() {
  FakeTransport tx;
  Station s("meteo-01", tx);
  s.begin(BOOT);
  s.on_connected();
  for (int i = 0; i < 3; ++i) s.on_sample(1000 * i, 20, 50, 1000);
  TEST_ASSERT_EQUAL(4, tx.sent.size());
  TEST_ASSERT_EQUAL_STRING("plant/meteo-01/status", tx.sent[0].topic.c_str());
  TEST_ASSERT_TRUE(tx.sent[0].retain);
  TEST_ASSERT_EQUAL_STRING("plant/meteo-01/telemetry", tx.sent[1].topic.c_str());
  TEST_ASSERT_FALSE(tx.sent[1].retain);
  auto q = tx.seqs();
  TEST_ASSERT_EQUAL_UINT32(1, q[0]);
  TEST_ASSERT_EQUAL_UINT32(3, q[2]);
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().published_live);
  TEST_ASSERT_EQUAL_UINT32(0, s.backlog());
}

void test_outage_is_stored_and_forwarded_oldest_first() {
  FakeTransport tx;
  Station s("meteo-01", tx, 20);
  s.begin(BOOT);
  s.on_sample(0, 20, 50, 1000);         // seq 1 live
  tx.up = false;
  for (int i = 0; i < 50; ++i) s.on_sample(1000 + i, 20, 50, 1000);   // seq 2..51 offline
  TEST_ASSERT_EQUAL_UINT32(50, s.backlog());
  tx.up = true;
  s.on_sample(9999, 20, 50, 1000);      // seq 52 arrives while the backlog is pending: must wait its turn
  s.loop();                              // budget 20
  TEST_ASSERT_EQUAL_UINT32(31, s.backlog());
  s.loop();
  s.loop();
  TEST_ASSERT_EQUAL_UINT32(0, s.backlog());
  auto q = tx.seqs();
  TEST_ASSERT_EQUAL(52, q.size());
  for (unsigned i = 0; i < q.size(); ++i) TEST_ASSERT_EQUAL_UINT32(i + 1, q[i]);   // strictly in order, no gaps
  TEST_ASSERT_EQUAL_UINT32(51, s.stats().published_late);
}

void test_outage_longer_than_history_counts_the_loss() {
  FakeTransport tx;
  Station s("meteo-01", tx, 1000);
  s.begin(BOOT);
  tx.up = false;
  const uint32_t n = kHistory + 100;
  for (uint32_t i = 0; i < n; ++i) s.on_sample(i, 20, 50, 1000);
  tx.up = true;
  s.loop();
  TEST_ASSERT_EQUAL_UINT32(100, s.stats().lost_overwritten);
  TEST_ASSERT_EQUAL_UINT32(kHistory, s.stats().published_late);
  TEST_ASSERT_EQUAL_UINT32(101, tx.seqs().front());
  TEST_ASSERT_EQUAL_UINT32(n, tx.seqs().back());
}

void test_failed_publish_is_retried_not_skipped() {
  FakeTransport tx;
  Station s("meteo-01", tx);
  s.begin(BOOT);
  tx.fail_next = 1;
  s.on_sample(0, 20, 50, 1000);   // refused by the stack
  s.on_sample(1, 20, 50, 1000);   // must not overtake seq 1
  TEST_ASSERT_EQUAL(0, tx.sent.size());
  s.loop();
  auto q = tx.seqs();
  TEST_ASSERT_EQUAL(2, q.size());
  TEST_ASSERT_EQUAL_UINT32(1, q[0]);
  TEST_ASSERT_EQUAL_UINT32(2, q[1]);
}

void test_resend_request_is_served_and_merged() {
  FakeTransport tx;
  Station s("meteo-01", tx, 100);
  s.begin(BOOT);
  for (int i = 0; i < 10; ++i) s.on_sample(i, 20, 50, 1000);
  tx.sent.clear();
  resend(s, "{\"boot\":1763650000000,\"from\":3,\"to\":4}");
  resend(s, "{\"boot\":1763650000000,\"from\":5,\"to\":5}");    // touches the previous range: merged
  resend(s, "{\"boot\":1763650000000,\"from\":3,\"to\":4}");    // server repeats itself: no duplicate work
  s.loop();
  auto q = tx.seqs();
  TEST_ASSERT_EQUAL(3, q.size());
  TEST_ASSERT_EQUAL_UINT32(3, q[0]);
  TEST_ASSERT_EQUAL_UINT32(5, q[2]);
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().resent);
}

void test_resend_for_other_boot_or_future_is_ignored() {
  FakeTransport tx;
  Station s("meteo-01", tx);
  s.begin(BOOT);
  for (int i = 0; i < 5; ++i) s.on_sample(i, 20, 50, 1000);
  tx.sent.clear();
  resend(s, "{\"boot\":1,\"from\":1,\"to\":2}");                   // previous power-up
  resend(s, "{\"boot\":1763650000000,\"from\":9,\"to\":12}");      // not sampled yet
  resend(s, "{\"boot\":1763650000000,\"from\":2,\"to\"");          // truncated
  s.on_message("plant/other/resend", reinterpret_cast<const uint8_t*>("{}"), 2);   // not ours: silently dropped
  s.loop();
  TEST_ASSERT_EQUAL(0, tx.sent.size());
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().resend_ignored);
}

void test_resend_beyond_history_is_reported_unavailable() {
  FakeTransport tx;
  Station s("meteo-01", tx, 1000);
  s.begin(BOOT);
  for (uint32_t i = 0; i < kHistory + 10; ++i) s.on_sample(i, 20, 50, 1000);
  tx.sent.clear();
  resend(s, "{\"boot\":1763650000000,\"from\":1,\"to\":12}");
  s.loop();
  TEST_ASSERT_EQUAL_UINT32(10, s.stats().resend_unavailable);
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().resent);
}

void test_resend_waits_while_offline() {
  FakeTransport tx;
  Station s("meteo-01", tx);
  s.begin(BOOT);
  for (int i = 0; i < 5; ++i) s.on_sample(i, 20, 50, 1000);
  resend(s, "{\"boot\":1763650000000,\"from\":2,\"to\":3}");
  tx.up = false;
  s.loop();
  tx.up = true;
  tx.sent.clear();
  s.loop();
  TEST_ASSERT_EQUAL(2, tx.sent.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_iso8601_known_instants);
  RUN_TEST(test_telemetry_payload);
  RUN_TEST(test_rejected_values_are_null_not_zero);
  RUN_TEST(test_payload_that_does_not_fit_fails_cleanly);
  RUN_TEST(test_parse_resend_accepts_the_servers_format_in_any_order);
  RUN_TEST(test_parse_resend_rejects_garbage);
  RUN_TEST(test_topics);
  RUN_TEST(test_ring_overwrites_oldest);
  RUN_TEST(test_spike_filter_removes_single_spike_and_rejects_out_of_range);
  RUN_TEST(test_backoff_doubles_to_cap_with_jitter_and_resets);
  RUN_TEST(test_simulated_sensor_stays_within_limits);
  RUN_TEST(test_live_publishing_in_order);
  RUN_TEST(test_outage_is_stored_and_forwarded_oldest_first);
  RUN_TEST(test_outage_longer_than_history_counts_the_loss);
  RUN_TEST(test_failed_publish_is_retried_not_skipped);
  RUN_TEST(test_resend_request_is_served_and_merged);
  RUN_TEST(test_resend_for_other_boot_or_future_is_ignored);
  RUN_TEST(test_resend_beyond_history_is_reported_unavailable);
  RUN_TEST(test_resend_waits_while_offline);
  return UNITY_END();
}
