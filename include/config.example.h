// Copy to include/config.h (git-ignored) and fill in. The build fails with a clear message if it's missing.
#pragma once

#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"

// The machine running the MQTT IoT Panel (broker on 1883). Use its LAN IP, not localhost.
#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_USER ""        // empty = anonymous
#define MQTT_PASSWORD ""

#define DEVICE_ID "meteo-01"           // topic plant/<DEVICE_ID>/...
#define SAMPLE_PERIOD_MS 2000

// BME280 on the default I2C pins (SDA 21, SCL 22). Address 0x76 (SDO to GND) or 0x77.
#define BME280_ADDRESS 0x76
// With no sensor found, publish simulated readings instead of nothing (handy to test the link first).
#define SIMULATE_IF_NO_SENSOR 1

#define NTP_SERVER "pool.ntp.org"
