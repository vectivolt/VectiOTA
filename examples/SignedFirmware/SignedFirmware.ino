// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------
//
// SignedFirmware — VectiOTA with HMAC-SHA256 signature verification.
//
// 1. Generate a 32-byte key once:        openssl rand -hex 32
// 2. Embed the hex string in `KEY`       (or read from NVS at boot).
// 3. CI signs the firmware before upload:
//      SIG=$(openssl dgst -sha256 -mac HMAC -macopt hexkey:KEY -hex firmware.bin \
//            | awk '{print $2}')
//      curl -u admin:vecti -X POST http://device/ota/upload?mode=firmware \
//           -H "X-Vecti-Signature: $SIG" -F update=@firmware.bin
//
// Unsigned uploads (or wrong signature) are rejected with HTTP 400 and the
// updater is aborted, so nothing is left half-written. The digest is computed
// incrementally as the body streams in and compared in constant time.
//
// Note: the browser UI does not sign, so with a key set /ota/upload is a
// CI-only endpoint. Drag-and-drop will come back 400 sig-missing.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <VectiOTA.h>

// CHANGE this to your own key. Empty string disables signature checks.
constexpr const char *KEY =
  "a9f1c0e7b9a3f6d8c2b4e6a8d0c2f4a6e8b0d2c4f6a8b0c2e4f6a8b0c2d4e6f8";

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(200);

  VectiOTA.setID(WiFi.macAddress());
  VectiOTA.setFWVersion("1.0.0-signed");
  VectiOTA.setTitle("Production OTA (signed)");
  VectiOTA.setSigningKey(KEY);             // ⇐ enable signature checks
  VectiOTA.setRateLimitMs(5000);
  VectiOTA.setRollbackTimeoutMs(30000);    // 30 s grace before auto-revert

  VectiOTA.onStart    ([](vecti::OtaMode m){ Serial.println("OTA started"); });
  VectiOTA.onEnd      ([](bool ok, const String &m){ Serial.printf("OTA end ok=%d %s\n", ok, m.c_str()); });
  VectiOTA.onError    ([](const String &r){ Serial.printf("OTA error: %s\n", r.c_str()); });

  VectiOTA.begin(&server, "admin", "strong-password");
  server.begin();

  if (runSelfTest()) VectiOTA.commit();
  else               Serial.println("self-test failed — bootloader will roll back");
}

bool runSelfTest() {
  // Real systems: ping the cloud, check sensors, etc.
  return true;
}

void loop() { VectiOTA.loop(); }
