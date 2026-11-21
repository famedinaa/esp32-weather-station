// Estación ESP32 firmware.
//
//   sensorTask (core 1, every SAMPLE_PERIOD_MS)  BME280 -> spike filter -> queue
//   loop()     (Arduino loop task)              Wi-Fi/MQTT with backoff, drains the queue into Station,
//                                                Station::loop() sends the backlog and resends
//
// All the protocol behaviour (numbering, store and forward, resend) lives in lib/station_core, which is
// unit tested on the PC. This file is only the glue to the ESP32: Wi-Fi, PubSubClient, BME280, NTP,
// FreeRTOS and the task watchdog.
#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy include/config.example.h to include/config.h and set your Wi-Fi and broker"
#endif

#include "protocol.h"
#include "sensor_logic.h"
#include "station.h"

using namespace station;

namespace {

constexpr uint32_t kWatchdogSeconds = 30;   // a task that doesn't check in for 30 s reboots the board

struct Sample {
  int64_t ts_ms;
  float temp, hum, pres;
};

WiFiClient net;
PubSubClient mqtt(net);
Adafruit_BME280 bme;
bool have_sensor = false;
QueueHandle_t samples;

class PubSubTransport : public Transport {
 public:
  bool connected() const override { return const_cast<PubSubClient&>(mqtt).connected(); }
  bool publish(const char* topic, const char* payload, bool retain) override {
    return mqtt.publish(topic, payload, retain);   // QoS 0: lost messages come back through resend
  }
};

PubSubTransport transport;
Station st(DEVICE_ID, transport);
Backoff wifi_backoff(1000, 30000), mqtt_backoff(1000, 60000);
uint32_t next_wifi_try = 0, next_mqtt_try = 0;
char topic_status[64], topic_resend[64], client_id[48];

int64_t epoch_ms() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

bool clock_is_set() { return epoch_ms() > 1700000000000LL; }   // after Nov 2023: NTP answered

float rnd01() { return static_cast<float>(esp_random() >> 8) / 16777216.0f; }

void watchdog_init() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = kWatchdogSeconds * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(kWatchdogSeconds, true);
#endif
}

void sensorTask(void*) {
  esp_task_wdt_add(nullptr);
  SpikeFilter ft(kTempLimits), fh(kHumLimits), fp(kPresLimits);
  SimulatedSensor sim(static_cast<uint32_t>(esp_random()));
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    esp_task_wdt_reset();
    Sample s{epoch_ms(), NAN, NAN, NAN};
    if (have_sensor) {
      s.temp = ft.update(bme.readTemperature());
      s.hum = fh.update(bme.readHumidity());
      s.pres = fp.update(bme.readPressure() / 100.0f);
    } else if (SIMULATE_IF_NO_SENSOR) {
      sim.read(s.ts_ms, s.temp, s.hum, s.pres);
    }
    if (xQueueSend(samples, &s, 0) != pdTRUE) log_w("sample queue full, dropping a reading");
  }
}

void on_mqtt_message(char* topic, uint8_t* payload, unsigned int len) { st.on_message(topic, payload, len); }

void keep_wifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifi_backoff.reset();
    return;
  }
  if (millis() < next_wifi_try) return;
  log_i("Wi-Fi: connecting to %s", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  next_wifi_try = millis() + wifi_backoff.next(rnd01()) + 5000;   // give the association time
}

void keep_mqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected() || millis() < next_mqtt_try) return;
  const bool anon = strlen(MQTT_USER) == 0;
  // Last will: if the link dies without a DISCONNECT, the broker publishes "offline" (retained) for us.
  const bool ok = mqtt.connect(client_id, anon ? nullptr : MQTT_USER, anon ? nullptr : MQTT_PASSWORD, topic_status,
                               1, true, "offline");
  if (!ok) {
    const uint32_t wait = mqtt_backoff.next(rnd01());
    log_w("MQTT: connect failed (state %d), retry in %u ms", mqtt.state(), wait);
    next_mqtt_try = millis() + wait;
    return;
  }
  mqtt_backoff.reset();
  mqtt.subscribe(topic_resend, 1);
  st.on_connected();
  log_i("MQTT: connected, backlog %u readings", st.backlog());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  log_i("Estación ESP32, device %s", DEVICE_ID);
  watchdog_init();
  esp_task_wdt_add(nullptr);   // the loop task

  make_topic(DEVICE_ID, "status", topic_status, sizeof topic_status);
  make_topic(DEVICE_ID, "resend", topic_resend, sizeof topic_resend);
  snprintf(client_id, sizeof client_id, "%s-%08x", DEVICE_ID, static_cast<unsigned>(ESP.getEfuseMac()));

  Wire.begin();
  have_sensor = bme.begin(BME280_ADDRESS, &Wire);
  if (have_sensor) {
    // Weather-monitoring settings from the datasheet (section 3.5.1): 1x oversampling, no IIR filter;
    // normal mode so a fresh reading is always waiting when the task wakes up.
    bme.setSampling(Adafruit_BME280::MODE_NORMAL, Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::FILTER_OFF, Adafruit_BME280::STANDBY_MS_1000);
  }
  log_i("BME280 %s", have_sensor ? "found" : (SIMULATE_IF_NO_SENSOR ? "not found: simulated readings" : "not found"));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // we drive reconnects with backoff
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(on_mqtt_message);
  mqtt.setKeepAlive(15);
  mqtt.setBufferSize(512);

  // `boot` must be the epoch time of this power-up (the server orders power-ups by it), so wait for NTP.
  configTime(0, 0, NTP_SERVER);
  while (!clock_is_set()) {
    keep_wifi();
    esp_task_wdt_reset();
    delay(200);
  }
  st.begin(static_cast<uint64_t>(epoch_ms()));
  log_i("clock set, boot id %llu", static_cast<unsigned long long>(st.boot()));

  samples = xQueueCreate(32, sizeof(Sample));
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 2, nullptr, 1);
}

void loop() {
  esp_task_wdt_reset();
  keep_wifi();
  keep_mqtt();
  Sample s;
  while (xQueueReceive(samples, &s, 0) == pdTRUE) st.on_sample(s.ts_ms, s.temp, s.hum, s.pres);
  st.loop();
  mqtt.loop();
  static uint32_t last_log = 0;
  if (millis() - last_log > 60000) {
    last_log = millis();
    const Stats& k = st.stats();
    log_i("seq %u, live %u, late %u, resent %u, lost %u, heap %u", st.last_seq(), k.published_live,
          k.published_late, k.resent, k.lost_overwritten, ESP.getFreeHeap());
  }
  delay(10);
}
