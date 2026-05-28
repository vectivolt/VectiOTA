// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------

// JouleOTA — async over-the-air updater for ESP32 / ESP8266.
//
// Why this exists: ElegantOTA (the popular incumbent) is closed-source after
// v2, lacks pull-from-URL updates, no signed firmware, no rate limiting, and
// its auth is "HTTP Basic or nothing". JouleOTA is MIT-licensed and adds:
//
//   * Push OTA (drag-and-drop in the browser) AND pull OTA (firmware fetched
//     from a URL the device polls or is told about over HTTP).
//   * Optional HMAC-SHA256 signature verification on the uploaded image so
//     a stolen Wi-Fi password alone can't push a malicious binary.
//   * Firmware + filesystem (SPIFFS / LittleFS) update modes, switchable
//     from the same UI.
//   * Hardware ID + firmware version exposed on the page (and in /info) so
//     fleet operators can tell devices apart from a tab.
//   * Live progress via Server-Sent Events — every client tab updates in
//     real time, not just the one that started the upload.
//   * Rollback after reboot — if the new firmware fails to mark itself valid
//     within `setRollbackTimeoutMs()`, the bootloader reverts to the
//     previous slot.
//   * Rate limiting (default 1 upload-attempt per 5s per IP) so a brute-force
//     auth attempt can't burn the flash erase cycle counter.
//
// Usage (3 lines, same as ElegantOTA):
//
//     #include <ESPAsyncWebServer.h>
//     #include <JouleOTA.h>
//     AsyncWebServer server(80);
//     void setup() { server.begin(); JouleOTA.begin(&server, "admin", "joule"); }
//     void loop()  { JouleOTA.loop(); }   // only needed for rollback / pull-mode
//
// All UI assets are embedded in flash via PROGMEM — no LittleFS dependency
// for the library itself.

#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>

#if defined(ESP32)
  #include <Update.h>
  #include <esp_ota_ops.h>
  #include <esp_partition.h>
  #include <esp_system.h>
  #include <mbedtls/md.h>
#elif defined(ESP8266)
  #include <Updater.h>
  #include <bearssl/bearssl_hmac.h>
#else
  #error "JouleOTA requires ESP32 or ESP8266"
#endif

namespace joule {

// What kind of image is being uploaded — determines which Update.begin()
// overload runs and which partition the bytes land in.
enum class OtaMode : uint8_t {
  Firmware = 0,   // application slot (U_FLASH / U_OTA on ESP32)
  Filesystem = 1, // SPIFFS / LittleFS partition (U_SPIFFS)
};

// Authentication style. Most users will stick with Basic, but Token is
// useful for headless CI flows that POST firmware from a build pipeline
// without prompting for a password.
enum class OtaAuth : uint8_t {
  None  = 0,
  Basic = 1,   // HTTP Basic over the wire (use HTTPS in production)
  Token = 2,   // X-Joule-Token: <token> header
};

// Callbacks the host sketch can hook to react to lifecycle events.
using OtaStartCb     = std::function<void(OtaMode mode)>;
using OtaProgressCb  = std::function<void(size_t current, size_t total)>;
using OtaEndCb       = std::function<void(bool success, const String &message)>;
using OtaErrorCb     = std::function<void(const String &reason)>;
using OtaRebootCb    = std::function<bool()>; // return true to allow auto-reboot

class JouleOTAClass {
public:
  JouleOTAClass();

  // Mount the OTA endpoints onto an existing AsyncWebServer. `username` and
  // `password` are optional; pass empty strings to disable Basic auth.
  // Endpoints registered:
  //   GET  /ota                — drag-drop UI (PROGMEM)
  //   GET  /ota/info           — JSON: hwId, fwVersion, mode, partitions
  //   POST /ota/upload         — multipart firmware/fs upload
  //   POST /ota/pull           — body: {"url":"...","mode":"firmware"} → device fetches
  //   GET  /ota/events         — Server-Sent Events stream (progress)
  //   POST /ota/rollback       — mark current slot invalid + reboot to previous
  //   POST /ota/commit         — mark current slot valid (called after self-test)
  void begin(AsyncWebServer *server,
             const String &username = "",
             const String &password = "");

  // Must be called from loop() for pull-mode polling and rollback timing.
  // Cost is ~5µs when idle.
  void loop();

  // ---- configuration --------------------------------------------------

  void setAuth(OtaAuth mode, const String &userOrToken, const String &password = "");
  void clearAuth() { _auth = OtaAuth::None; _user = ""; _pass = ""; _token = ""; }

  // Free-form identity strings shown on the page and returned by /ota/info.
  // Recommended: setID(WiFi.macAddress()), setFWVersion(__DATE__ " " __TIME__).
  void setID(const String &id)            { _hwId = id; }
  void setFWVersion(const String &ver)    { _fwVersion = ver; }
  void setTitle(const String &title)      { _title = title; }
  void setBrandColor(const String &css)   { _brandColor = css; }

  // Optional HMAC-SHA256 signature check. If a key is set, each upload must
  // arrive with header `X-Joule-Signature: <hex>` over the entire body.
  // Empty key disables the check (default).
  void setSigningKey(const String &hexKey) { _signingKey = hexKey; }

  // Rate-limiting — minimum gap between accepted upload starts per remote IP.
  void setRateLimitMs(uint32_t ms)        { _rateLimitMs = ms; }

  // Disable a mode entirely (e.g. lock the device to firmware updates only).
  void allowFirmwareUpdates(bool enabled)   { _allowFirmware  = enabled; }
  void allowFilesystemUpdates(bool enabled) { _allowFilesystem = enabled; }
  void allowPullMode(bool enabled)          { _allowPull = enabled; }

  // Rollback support: after a successful flash + reboot, the new firmware
  // must call commit() before this timeout or the bootloader reverts.
  void setRollbackTimeoutMs(uint32_t ms)  { _rollbackTimeoutMs = ms; }
  void commit();                          // call from setup() after self-test
  void rollback();                        // force immediate revert + reboot

  // ---- callbacks ------------------------------------------------------
  void onStart(OtaStartCb cb)         { _onStart = std::move(cb); }
  void onProgress(OtaProgressCb cb)   { _onProgress = std::move(cb); }
  void onEnd(OtaEndCb cb)             { _onEnd = std::move(cb); }
  void onError(OtaErrorCb cb)         { _onError = std::move(cb); }
  // Return false from onBeforeReboot to suppress the automatic reboot after
  // a successful flash (e.g. so the sketch can finish a graceful shutdown).
  void onBeforeReboot(OtaRebootCb cb) { _onBeforeReboot = std::move(cb); }

  // ---- status ---------------------------------------------------------
  bool isUpdating()    const { return _updating; }
  size_t bytesWritten() const { return _written; }
  size_t totalBytes()   const { return _total; }
  uint8_t progressPct() const { return _total ? (uint8_t)((_written * 100ULL) / _total) : 0; }

private:
  // HTTP handlers (declared private so the lambdas in begin() can friend us).
  bool _authorize(AsyncWebServerRequest *req) const;
  bool _rateLimitOk(AsyncWebServerRequest *req);
  void _emitEvent(const String &type, const String &payload);
  bool _verifySignature(const uint8_t *body, size_t len, const String &hexSig) const;
  void _resetUploadState();

  // Pull-mode polling (called from loop()).
  void _processPullQueue();
  bool _executePull(const String &url, OtaMode mode);

  AsyncWebServer       *_server = nullptr;
  AsyncEventSource     *_events = nullptr;

  OtaAuth _auth = OtaAuth::None;
  String  _user, _pass, _token;

  String  _hwId       = "";
  String  _fwVersion  = "1.0.0";
  String  _title      = "JouleOTA";
  String  _brandColor = "#7c5cff";
  String  _signingKey = "";

  uint32_t _rateLimitMs       = 5000;
  bool     _allowFirmware     = true;
  bool     _allowFilesystem   = true;
  bool     _allowPull         = true;

  // Per-IP last-upload-attempt timestamp. Tiny ring of 8 entries — enough
  // for a small office network, not a DoS-resistant cache.
  struct RateSlot { uint32_t ip = 0; uint32_t lastMs = 0; };
  RateSlot _rate[8];

  volatile bool   _updating = false;
  size_t          _written  = 0;
  size_t          _total    = 0;
  OtaMode         _mode     = OtaMode::Firmware;

  uint32_t _rollbackTimeoutMs   = 0;
  uint32_t _rollbackArmedAtMs   = 0;
  bool     _rollbackArmed       = false;
  bool     _committed           = false;

  // Pull-mode pending request — set by /ota/pull, drained in loop().
  String   _pullUrl;
  OtaMode  _pullMode = OtaMode::Firmware;
  bool     _pullPending = false;

  OtaStartCb     _onStart;
  OtaProgressCb  _onProgress;
  OtaEndCb       _onEnd;
  OtaErrorCb     _onError;
  OtaRebootCb    _onBeforeReboot;
};

} // namespace joule

extern joule::JouleOTAClass JouleOTA;
