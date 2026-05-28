// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------

// Minimal JouleOTA example — Wi-Fi station + drag-drop OTA UI at /ota.
// Open http://<device-ip>/ota in a browser, drop a firmware.bin.
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleOTA.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin("YOUR_SSID","YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.println(); Serial.println(WiFi.localIP());

  JouleOTA.setID(WiFi.macAddress());
  JouleOTA.setFWVersion("1.0.0");
  JouleOTA.setTitle("JouleOTA Demo");
  JouleOTA.setRollbackTimeoutMs(30000);          // auto-revert after 30s if not committed
  JouleOTA.onStart([](joule::OtaMode m){ Serial.println("OTA start"); });
  JouleOTA.onEnd([](bool ok, const String &msg){ Serial.printf("OTA end ok=%d %s\n", ok, msg.c_str()); });

  JouleOTA.begin(&server, "admin", "joule");
  server.begin();

  JouleOTA.commit();                              // mark "this firmware is good"
}

void loop() { JouleOTA.loop(); }
