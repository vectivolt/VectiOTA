// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------
//
// PullFromUrl — device-initiated OTA. Every CHECK_EVERY_MS the firmware
// fetches a small JSON manifest from your build server, compares the
// version string against the running one, and if different, pulls the
// firmware binary and flashes it. Perfect for fleet rollouts without
// having to push from a CI server.
//
// Manifest format expected at MANIFEST_URL:
//   { "version": "1.2.3",
//     "url":     "https://builds.example.com/v1.2.3/firmware.bin",
//     "sha256":  "optional hex digest" }

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <VectiOTA.h>

constexpr const char *RUNNING_VERSION = "1.0.0";
constexpr const char *MANIFEST_URL    = "https://builds.example.com/vecti-device/manifest.json";
constexpr uint32_t    CHECK_EVERY_MS  = 60UL * 60UL * 1000UL;     // 1 h

AsyncWebServer server(80);

uint32_t lastCheck = 0;

void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http; http.begin(MANIFEST_URL);
  int code = http.GET();
  if (code != 200) { Serial.printf("manifest http %d\n", code); http.end(); return; }
  JsonDocument doc;
  if (deserializeJson(doc, http.getStream()) != DeserializationError::Ok) {
    Serial.println("manifest parse failed"); http.end(); return;
  }
  http.end();
  String v   = doc["version"] | "";
  String url = doc["url"]     | "";
  if (v.length() == 0 || url.length() == 0) return;
  if (v == RUNNING_VERSION) { Serial.printf("up to date (%s)\n", v.c_str()); return; }

  Serial.printf("new version %s — pulling %s\n", v.c_str(), url.c_str());
  // POST our own /ota/pull endpoint so all the standard auth, rate-limit
  // and SSE-progress paths fire — same as if the user had clicked Pull URL
  // from a browser.
  HTTPClient self;
  self.begin("http://127.0.0.1/ota/pull");
  self.setAuthorization("admin", "strong-password");   // same credentials begin() was given
  self.addHeader("Content-Type", "application/json");
  String body = String("{\"url\":\"") + url + "\",\"mode\":\"firmware\"}";
  self.POST(body);
  self.end();
}

void setup() {
  Serial.begin(115200);
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(200);

  VectiOTA.setID(WiFi.macAddress());
  VectiOTA.setFWVersion(RUNNING_VERSION);
  VectiOTA.setTitle("Fleet OTA · " + WiFi.macAddress());
  VectiOTA.allowPullMode(true);
  // https:// pulls are refused until you say what to trust. Pin the root CA
  // your build server's certificate chains to:
  //   VectiOTA.setPullCACert(MY_ROOT_CA_PEM);
  // The opt-out below accepts any certificate — fine on a lab LAN, not in a
  // fleet, where anyone who can answer DNS then owns the boot image.
  VectiOTA.allowInsecurePullTls(true);
  VectiOTA.begin(&server, "admin", "strong-password");
  server.begin();
  VectiOTA.commit();

  checkForUpdate();      // immediate check on boot
  lastCheck = millis();
}

void loop() {
  VectiOTA.loop();
  if (millis() - lastCheck >= CHECK_EVERY_MS) {
    lastCheck = millis();
    checkForUpdate();
  }
}
