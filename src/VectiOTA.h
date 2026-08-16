// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------

// VectiOTA — async over-the-air updater for ESP32 / ESP8266.
//
// Why this exists: ElegantOTA (the popular incumbent) is closed-source after
// v2, lacks pull-from-URL updates, no signed firmware, no rate limiting, and
// its auth is "HTTP Basic or nothing". VectiOTA is Apache-2.0 licensed and adds:
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
//     #include <VectiOTA.h>
//     AsyncWebServer server(80);
//     void setup() { server.begin(); VectiOTA.begin(&server, "admin", "vecti"); }
//     void loop()  { VectiOTA.loop(); }   // only needed for rollback / pull-mode
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
  #error "VectiOTA requires ESP32 or ESP8266"
#endif

namespace vecti {

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
  Token = 2,   // X-Vecti-Token: <token> header
};

// Callbacks the host sketch can hook to react to lifecycle events.
using OtaStartCb     = std::function<void(OtaMode mode)>;
using OtaProgressCb  = std::function<void(size_t current, size_t total)>;
using OtaEndCb       = std::function<void(bool success, const String &message)>;
using OtaErrorCb     = std::function<void(const String &reason)>;
using OtaRebootCb    = std::function<bool()>; // return true to allow auto-reboot

class VectiOTAClass {
public:
  VectiOTAClass();

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

  // Must be called from loop() for pull-mode polling, rollback timing and the
  // deferred reboot after a successful flash — a handler cannot reboot itself,
  // see the note on _rebootPending. Cost is ~5µs when idle.
  void loop();

  // ---- configuration --------------------------------------------------

  void setAuth(OtaAuth mode, const String &userOrToken, const String &password = "");
  // Goes through setAuth() so the /ota/events middleware is re-applied too —
  // otherwise the SSE stream keeps the old credentials (or, from Token mode,
  // AUTH_DENIED) and answers 401 on a device with auth switched off.
  void clearAuth() { setAuth(OtaAuth::None, "", ""); }

  // Free-form identity strings shown on the page and returned by /ota/info.
  // Recommended: setID(WiFi.macAddress()), setFWVersion(__DATE__ " " __TIME__).
  void setID(const String &id)            { _hwId = id; }
  void setFWVersion(const String &ver)    { _fwVersion = ver; }
  void setTitle(const String &title)      { _title = title; }
  void setBrandColor(const String &css)   { _brandColor = css; }

  // Optional HMAC-SHA256 signature check. If a key is set, each upload must
  // arrive with header `X-Vecti-Signature: <hex>` over the entire multipart
  // body — that is, over exactly the bytes the browser/curl sends as the file
  // part, which is the image itself. Empty key disables the check (default).
  // Applies to /ota/upload only; a pulled image carries no signature, so pin
  // a CA for it instead (setPullCACert).
  void setSigningKey(const String &hexKey) { _signingKey = hexKey; }

  // TLS policy for pull-from-URL. WiFiClientSecure verifies nothing by
  // default, so an https:// pull with neither of these set is refused rather
  // than silently accepting whatever answers the DNS query. `pemRootCa` is
  // not copied — pass a string literal or another pointer that outlives the
  // device.
  void setPullCACert(const char *pemRootCa) { _pullCaCert = pemRootCa; }
  void allowInsecurePullTls(bool enabled)   { _pullInsecure = enabled; }

  // Rate-limiting — minimum gap between accepted upload starts per remote IP.
  void setRateLimitMs(uint32_t ms)        { _rateLimitMs = ms; }

  // Disable a mode entirely (e.g. lock the device to firmware updates only).
  void allowFirmwareUpdates(bool enabled)   { _allowFirmware  = enabled; }
  void allowFilesystemUpdates(bool enabled) { _allowFilesystem = enabled; }
  void allowPullMode(bool enabled)          { _allowPull = enabled; }

  // Rollback support: after a successful flash + reboot, the new firmware
  // must call commit() before this timeout or the bootloader reverts. The
  // watchdog only arms when the bootloader is actually waiting on a verdict
  // for the running image, so a serially-flashed build is never affected.
  // Safe to call before or after begin().
  void setRollbackTimeoutMs(uint32_t ms);
  void commit();                          // call from setup() after self-test
  // Revert to the previous slot and reboot. No-op (emits `rollback-unavailable`)
  // when there is no other valid slot to go back to.
  void rollback();

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
  void _resetUploadState();
  // Records why an upload was refused so the completion handler can answer
  // with a real status instead of guessing from Update.hasError().
  void _rejectUpload(int status, const String &reason);
  void _scheduleReboot(uint32_t delayMs, bool viaRollback);
  void _applyEventsAuth();

  // Incremental HMAC-SHA256 over the upload body. The signature covers the
  // whole image, which is far larger than free RAM on either chip, so the
  // digest is fed chunk by chunk and only compared once the last chunk lands.
  bool _sigBegin();
  void _sigUpdate(const uint8_t *data, size_t len);
  bool _sigFinish(const String &expectedHex);
  void _sigAbort();

  // Pull-mode polling (called from loop()).
  void _processPullQueue();
  bool _executePull(const String &url, OtaMode mode);

  AsyncWebServer       *_server = nullptr;
  AsyncEventSource     *_events = nullptr;

  OtaAuth _auth = OtaAuth::None;
  String  _user, _pass, _token;

  String  _hwId       = "";
  String  _fwVersion  = "1.0.0";
  String  _title      = "VectiOTA";
  String  _brandColor = "#7c5cff";
  String  _signingKey = "";

  uint32_t _rateLimitMs       = 5000;
  bool     _allowFirmware     = true;
  bool     _allowFilesystem   = true;
  bool     _allowPull         = true;

  const char *_pullCaCert     = nullptr;
  bool        _pullInsecure   = false;

  // Per-IP last-upload-attempt timestamp. Tiny ring of 8 entries — enough
  // for a small office network, not a DoS-resistant cache.
  struct RateSlot { uint32_t ip = 0; uint32_t lastMs = 0; };
  RateSlot _rate[8];

  volatile bool   _updating = false;
  size_t          _written  = 0;
  size_t          _total    = 0;
  size_t          _lastEmit = 0;
  OtaMode         _mode     = OtaMode::Firmware;

  // Upload outcome, recorded by the chunk handler. Never infer success from
  // Update.hasError(): a refused upload never touches the updater at all, so
  // hasError() is false and the completion handler would answer 200 and
  // reboot into firmware that was never written.
  // All four are written and read only on the AsyncTCP task (chunk handler,
  // completion handler, onDisconnect). _uploadOwner is the request that took
  // the updater; any other request's completion handler must not read or
  // clear the rest. Compared as an identity only — never dereferenced.
  AsyncWebServerRequest *_uploadOwner = nullptr;
  bool     _uploadStarted = false;
  int      _uploadStatus  = 0;
  String   _uploadError;

  bool     _sigActive = false;
  String   _sigExpected;
#if defined(ESP32)
  mbedtls_md_context_t _sigCtx;
#elif defined(ESP8266)
  br_hmac_context      _sigCtx;
#endif

  uint32_t _rollbackTimeoutMs   = 0;
  uint32_t _rollbackArmedAtMs   = 0;
  bool     _rollbackArmed       = false;
  bool     _committed           = false;

  // Reboots are scheduled here and executed from loop(), never inline in a
  // handler: the AsyncTCP task is the one that writes the queued response, so
  // resetting (or delay()ing) inside a callback guarantees the client sees a
  // dropped socket instead of the 200 it was waiting for.
  uint32_t _rebootAtMs        = 0;
  bool     _rebootPending     = false;
  bool     _rebootIsRollback  = false;

  // Pull-mode pending request — set by /ota/pull on the AsyncTCP task,
  // drained in loop(). _pullUrl is only written while _pullPending is false
  // and only read while it is true, so the flag alone orders the handoff.
  String            _pullUrl;
  String            _pullBody;
  String            _pullError;
  OtaMode           _pullMode = OtaMode::Firmware;
  volatile bool     _pullPending = false;
  // Latched verdict for the response handler (AsyncTCP task only). Separate
  // from _pullPending, which loop() clears as soon as it starts the download.
  bool              _pullAccepted = false;

  OtaStartCb     _onStart;
  OtaProgressCb  _onProgress;
  OtaEndCb       _onEnd;
  OtaErrorCb     _onError;
  OtaRebootCb    _onBeforeReboot;
};

} // namespace vecti

extern vecti::VectiOTAClass VectiOTA;
