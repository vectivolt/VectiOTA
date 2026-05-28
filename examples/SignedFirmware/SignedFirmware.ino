// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------
//
// SignedFirmware — JouleOTA with HMAC-SHA256 signature verification.
//
// 1. Generate a 32-byte key once:        openssl rand -hex 32
// 2. Embed the hex string in `KEY`       (or read from NVS at boot).
// 3. CI signs the firmware before upload:
//      SIG=$(openssl dgst -sha256 -mac HMAC -macopt hexkey:KEY -hex firmware.bin \
//            | awk '{print $2}')
//      curl -u admin:joule -X POST http://device/ota/upload?mode=firmware \
//           -H "X-Joule-Signature: $SIG" -F update=@firmware.bin
//
// Unsigned uploads (or wrong signature) are rejected with HTTP 400.
// A constant-time hex compare defeats timing-oracle attacks.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleOTA.h>

// CHANGE this to your own key. Empty string disables signature checks.
constexpr const char *KEY =
  "a9f1c0e7b9a3f6d8c2b4e6a8d0c2f4a6e8b0d2c4f6a8b0c2e4f6a8b0c2d4e6f8";

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(200);

  JouleOTA.setID(WiFi.macAddress());
  JouleOTA.setFWVersion("1.0.0-signed");
  JouleOTA.setTitle("Production OTA (signed)");
  JouleOTA.setSigningKey(KEY);             // ⇐ enable signature checks
  JouleOTA.setRateLimitMs(5000);
  JouleOTA.setRollbackTimeoutMs(30000);    // 30 s grace before auto-revert

  JouleOTA.onStart    ([](joule::OtaMode m){ Serial.println("OTA started"); });
  JouleOTA.onEnd      ([](bool ok, const String &m){ Serial.printf("OTA end ok=%d %s\n", ok, m.c_str()); });
  JouleOTA.onError    ([](const String &r){ Serial.printf("OTA error: %s\n", r.c_str()); });

  JouleOTA.begin(&server, "admin", "strong-password");
  server.begin();

  if (runSelfTest()) JouleOTA.commit();
  else               Serial.println("self-test failed — bootloader will roll back");
}

bool runSelfTest() {
  // Real systems: ping the cloud, check sensors, etc.
  return true;
}

void loop() { JouleOTA.loop(); }
