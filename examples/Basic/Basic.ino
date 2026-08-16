// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: Chinmoy Bhuyan
// Email:  chinmoy@joulepoint.com
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------

// Minimal VectiOTA example — Wi-Fi station + drag-drop OTA UI at /ota.
// Open http://<device-ip>/ota in a browser, drop a firmware.bin.
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <VectiOTA.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin("YOUR_SSID","YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.println(); Serial.println(WiFi.localIP());

  VectiOTA.setID(WiFi.macAddress());
  VectiOTA.setFWVersion("1.0.0");
  VectiOTA.setTitle("VectiOTA Demo");
  VectiOTA.setRollbackTimeoutMs(30000);          // auto-revert after 30s if not committed
  VectiOTA.onStart([](vecti::OtaMode m){ Serial.println("OTA start"); });
  VectiOTA.onEnd([](bool ok, const String &msg){ Serial.printf("OTA end ok=%d %s\n", ok, msg.c_str()); });

  VectiOTA.begin(&server, "admin", "vecti");
  server.begin();

  VectiOTA.commit();                              // mark "this firmware is good"
}

void loop() { VectiOTA.loop(); }
