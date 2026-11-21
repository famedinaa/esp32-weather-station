// The firmware's Station class running on a PC, for the end-to-end demo with no board.
//
// The same lib/station_core sources the ESP32 runs are compiled here. What the ESP32 does with Wi-Fi and
// PubSubClient is done by host/bridge.mjs, which talks to this process over stdin/stdout, one command
// per line:
//
//   in   SAMPLE <epoch_ms>            take a (simulated) reading
//        LOOP                         run Station::loop()
//        UP | DOWN                    the MQTT link came up / went down
//        MSG <topic> <payload>        a message on a subscribed topic
//        STATS                        print counters
//   out  PUB <retain 0|1> <topic> <payload>
//        STATS {...}
//        OK                           end of the response to one command
#include <cstdio>
#include <iostream>
#include <string>

#include "sensor_logic.h"
#include "station.h"

using namespace station;

namespace {

class LineTransport : public Transport {
 public:
  bool up = false;
  bool connected() const override { return up; }
  bool publish(const char* topic, const char* payload, bool retain) override {
    if (!up) return false;
    std::printf("PUB %d %s %s\n", retain ? 1 : 0, topic, payload);
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: station_host <device_id> <boot_epoch_ms>\n");
    return 2;
  }
  const std::string id = argv[1];
  LineTransport tx;
  Station st(id.c_str(), tx);
  st.begin(std::stoull(argv[2]));
  SimulatedSensor sensor;
  SpikeFilter ft(kTempLimits), fh(kHumLimits), fp(kPresLimits);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("SAMPLE ", 0) == 0) {
      const int64_t ts = std::stoll(line.substr(7));
      float t, h, p;
      sensor.read(ts, t, h, p);
      st.on_sample(ts, ft.update(t), fh.update(h), fp.update(p));
    } else if (line == "LOOP") {
      st.loop();
    } else if (line == "UP") {
      tx.up = true;
      st.on_connected();
    } else if (line == "DOWN") {
      tx.up = false;
    } else if (line.rfind("MSG ", 0) == 0) {
      const size_t sp = line.find(' ', 4);
      if (sp != std::string::npos) {
        const std::string topic = line.substr(4, sp - 4), payload = line.substr(sp + 1);
        st.on_message(topic.c_str(), reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
      }
    } else if (line == "STATS") {
      const Stats& k = st.stats();
      std::printf(
          "STATS {\"seq\":%u,\"backlog\":%u,\"sampled\":%u,\"live\":%u,\"late\":%u,\"resent\":%u,\"lost\":%u,"
          "\"unavailable\":%u,\"ignored\":%u}\n",
          st.last_seq(), st.backlog(), k.sampled, k.published_live, k.published_late, k.resent, k.lost_overwritten,
          k.resend_unavailable, k.resend_ignored);
    }
    std::printf("OK\n");
    std::fflush(stdout);
  }
  return 0;
}
