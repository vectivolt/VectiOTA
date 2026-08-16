// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------

// VectiOTA implementation. See VectiOTA.h for the rationale + public API.
//
// The interesting code paths in this file:
//
//   * upload handler        — assembled out of three callbacks because
//                             AsyncWebServer splits multipart bodies into
//                             (header) + (chunks) + (trailer). We treat the
//                             whole upload as a single Update.write() stream
//                             and only call Update.end() when isFinal is set.
//
//   * pull handler          — does NOT do the HTTP fetch inline (Async tasks
//                             must stay non-blocking). Instead it queues the
//                             URL and loop() picks it up on the next tick.
//
//   * rollback / commit     — wraps the ESP-IDF esp_ota_mark_app_valid* /
//                             esp_ota_mark_app_invalid* calls and adds a
//                             watchdog timer: if the new firmware boots but
//                             never calls commit() within the configured
//                             window, the bootloader picks the old slot on
//                             the next reset.

#include "VectiOTA.h"
#include "VectiOTA_ui_gz.h"
#include <ArduinoJson.h>

// Serve pre-compressed UI with Content-Encoding: gzip — browsers inflate
// transparently and the 70 KB SPA drops to 25 KB on the wire, which is what
// makes it survive weak-RSSI links where the uncompressed response stalls
// mid-stream. Only the compressed copy is in flash, so a client that doesn't
// advertise gzip gets it anyway; there is nothing to fall back to.
static void sendGzippedUi(AsyncWebServerRequest *req, const uint8_t *gz, size_t len, bool credentialed) {
  AsyncWebServerResponse *res = req->beginResponse(200, "text/html; charset=utf-8", gz, len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Vary", "Accept-Encoding");
  // A shared cache must not hand an auth-gated page body to the next visitor.
  res->addHeader("Cache-Control", credentialed ? "private, max-age=3600" : "public, max-age=3600");
  req->send(res);
}

// Pull-from-URL is ESP32-only, so the HTTP client only gets pulled in there.
#if defined(ESP32)
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
  // arduino-esp32 3.x re-homed WiFiClient onto NetworkClient and stopped
  // pulling it in transitively, so 2.x builds compiled without this and 3.x
  // (which is what the C6 and H2 need) does not. Include it explicitly rather
  // than relying on a transitive path that already changed once.
  #include <WiFiClient.h>
#endif

namespace vecti {

VectiOTAClass::VectiOTAClass() = default;

// A pull request is a couple of hundred bytes of JSON; anything larger is
// either a mistake or an attempt to grow the accumulator past the free heap.
static constexpr size_t   kPullBodyMax   = 512;
// Whole-download ceiling and "server went quiet" ceiling for the pull loop.
// Without both, a keep-alive socket that has finished sending never reports
// disconnected and loop() spins forever.
static constexpr uint32_t kPullTotalMs   = 5UL * 60UL * 1000UL;
static constexpr uint32_t kPullIdleMs    = 10UL * 1000UL;
// _executePull has no ESP8266 implementation, so /ota/pull refuses there
// instead of answering 202 for a rollout that will never happen.
#if defined(ESP32)
static constexpr bool     kPullSupported = true;
#else
static constexpr bool     kPullSupported = false;
#endif

// ---------- helpers ---------------------------------------------------------

// The two cores spell the updater's error accessor differently, and ESP8266
// has no abort() at all — end(false) refuses to finish an incomplete image and
// resets the updater, which is the same thing.
#if defined(ESP32)
static String updErr()   { return String(Update.errorString()); }
static void   updAbort() { Update.abort(); }
#else
static String updErr()   { return Update.getErrorString(); }
static void   updAbort() { Update.end(false); }
#endif

static String contentTypeFromMode(OtaMode m) {
  return (m == OtaMode::Filesystem) ? F("filesystem") : F("firmware");
}

// Strict hex decode: returns 0 on any non-hex character or odd length, so a
// malformed key or signature can never decode to a short-but-valid buffer.
static size_t hexToBytes(const String &hex, uint8_t *out, size_t maxOut) {
  size_t n = 0;
  if (hex.length() == 0 || (hex.length() & 1)) return 0;
  for (size_t i = 0; i + 1 < hex.length(); i += 2) {
    if (n >= maxOut) return 0;
    int v = 0;
    for (int k = 0; k < 2; k++) {
      char c = hex[i + k];
      int d = (c >= '0' && c <= '9') ? c - '0'
            : ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') ? (c | 0x20) - 'a' + 10 : -1;
      if (d < 0) return 0;
      v = (v << 4) | d;
    }
    out[n++] = (uint8_t)v;
  }
  return n;
}

// Whether the bootloader is still waiting on a verdict for the image we are
// running. Anything else (serial flash, factory partition, single-slot layout,
// ESP8266) has nothing to roll back to, so arming the watchdog would only
// produce a reset every timeout period, forever.
static bool runningImagePendingVerify() {
#if defined(ESP32)
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  return running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY && esp_ota_check_rollback_is_possible();
#else
  return false;
#endif
}

bool VectiOTAClass::_sigBegin() {
  uint8_t key[64];
  size_t keyLen = hexToBytes(_signingKey, key, sizeof(key));
  if (keyLen == 0) return false;
#if defined(ESP32)
  mbedtls_md_init(&_sigCtx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&_sigCtx, info, 1) != 0 ||
      mbedtls_md_hmac_starts(&_sigCtx, key, keyLen) != 0) {
    mbedtls_md_free(&_sigCtx);
    return false;
  }
#else
  // BearSSL copies the derived inner/outer key blocks into the HMAC context,
  // so the key context may live on the stack.
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, &br_sha256_vtable, key, keyLen);
  br_hmac_init(&_sigCtx, &kc, 0);
#endif
  _sigActive = true;
  return true;
}

void VectiOTAClass::_sigUpdate(const uint8_t *data, size_t len) {
  if (!_sigActive || !len) return;
#if defined(ESP32)
  mbedtls_md_hmac_update(&_sigCtx, data, len);
#else
  br_hmac_update(&_sigCtx, data, len);
#endif
}

bool VectiOTAClass::_sigFinish(const String &expectedHex) {
  if (!_sigActive) return false;
  uint8_t mac[32];
#if defined(ESP32)
  mbedtls_md_hmac_finish(&_sigCtx, mac);
  mbedtls_md_free(&_sigCtx);
#else
  br_hmac_out(&_sigCtx, mac);
#endif
  _sigActive = false;

  uint8_t want[32];
  if (hexToBytes(expectedHex, want, sizeof(want)) != sizeof(want)) return false;
  // Constant-time compare so a remote attacker can't byte-walk the digest off
  // the timing curve.
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(mac); i++) diff |= (uint8_t)(mac[i] ^ want[i]);
  return diff == 0;
}

void VectiOTAClass::_sigAbort() {
#if defined(ESP32)
  if (_sigActive) mbedtls_md_free(&_sigCtx);
#endif
  _sigActive = false;
  _sigExpected = "";
}

bool VectiOTAClass::_authorize(AsyncWebServerRequest *req) const {
  if (_auth == OtaAuth::None) return true;
  if (_auth == OtaAuth::Basic) {
    if (_user.length()==0) return true;
    return req->authenticate(_user.c_str(), _pass.c_str());
  }
  if (_auth == OtaAuth::Token) {
    if (!req->hasHeader("X-Vecti-Token")) return false;
    return req->getHeader("X-Vecti-Token")->value() == _token;
  }
  return false;
}

bool VectiOTAClass::_rateLimitOk(AsyncWebServerRequest *req) {
  uint32_t now = millis();
  // IPAddress -> uint32_t is platform-specific; treat as opaque key.
  uint32_t ip = (uint32_t)req->client()->remoteIP();
  RateSlot *empty = nullptr, *oldest = &_rate[0];
  for (auto &s : _rate) {
    if (s.ip == ip) {
      if ((now - s.lastMs) < _rateLimitMs) return false;
      s.lastMs = now; return true;
    }
    if (s.ip == 0 && !empty) empty = &s;
    if (s.lastMs < oldest->lastMs) oldest = &s;
  }
  RateSlot *target = empty ? empty : oldest;
  target->ip = ip; target->lastMs = now; return true;
}

void VectiOTAClass::_emitEvent(const String &type, const String &payload) {
  if (_events) _events->send(payload.c_str(), type.c_str(), millis());
}

// AsyncTCP task ONLY — it touches _uploadOwner/_uploadStarted/_uploadStatus/
// _uploadError, which that task owns. The loop task (_executePull) must not
// call this; it clears _updating itself instead.
void VectiOTAClass::_resetUploadState() {
  // Dropping _updating without closing the updater orphans the session, and
  // every later Update.begin() then fails with "already running" until power
  // cycle. Abort here so no caller can forget.
  if (_updating) updAbort();
  _sigAbort();
  _updating = false; _written = 0; _total = 0; _lastEmit = 0;
  _uploadStarted = false; _uploadStatus = 0; _uploadError = "";
  _uploadOwner = nullptr;
}

void VectiOTAClass::_rejectUpload(int status, const String &reason) {
  // Whatever the updater had open is dead now — leaving it running would make
  // every later Update.begin() fail with "already running" until power cycle.
  if (_updating) { updAbort(); _updating = false; }
  _sigAbort();
  _uploadStatus = status;
  _uploadError  = reason;
  _emitEvent("status", reason);
  if (_onError) _onError(reason);
}

void VectiOTAClass::_scheduleReboot(uint32_t delayMs, bool viaRollback) {
  _rebootAtMs       = millis() + delayMs;
  _rebootIsRollback = viaRollback;
  _rebootPending    = true;
}

// ---------- public API ------------------------------------------------------

void VectiOTAClass::setAuth(OtaAuth mode, const String &userOrToken, const String &password) {
  _auth = mode;
  if (mode == OtaAuth::Basic) { _user = userOrToken; _pass = password; _token = ""; }
  else if (mode == OtaAuth::Token) { _token = userOrToken; _user = _pass = ""; }
  else { _user = _pass = _token = ""; }
  _applyEventsAuth();
}

// The SSE stream answers before any request callback of ours runs, so
// _authorize() can never see it — the credentials have to live in the
// handler's own auth middleware instead. Re-applied whenever the auth config
// changes so it doesn't matter whether setAuth() precedes or follows begin().
void VectiOTAClass::_applyEventsAuth() {
  if (!_events) return;
  if (_auth == OtaAuth::Basic && _user.length()) {
    _events->setAuthentication(_user.c_str(), _pass.c_str(), AsyncAuthType::AUTH_BASIC);
  } else if (_auth == OtaAuth::Token) {
    // Browsers cannot attach X-Vecti-Token to an EventSource, so there is no
    // way to authenticate this stream in token mode. Deny it rather than leave
    // the one unauthenticated hole in an otherwise gated API.
    _events->setAuthentication("", "", AsyncAuthType::AUTH_DENIED);
  } else {
    _events->setAuthentication("", "", AsyncAuthType::AUTH_NONE);
  }
}

void VectiOTAClass::commit() {
#if defined(ESP32)
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }
#endif
  _committed = true; _rollbackArmed = false;
}

void VectiOTAClass::setRollbackTimeoutMs(uint32_t ms) {
  _rollbackTimeoutMs = ms;
  // Order-independent: begin() may already have run, and a timeout set after
  // it used to arm nothing at all.
  if (ms > 0 && !_rollbackArmed && !_committed && runningImagePendingVerify()) {
    _rollbackArmedAtMs = millis();
    _rollbackArmed = true;
  }
}

void VectiOTAClass::rollback() {
#if defined(ESP32)
  if (esp_ota_check_rollback_is_possible()) {
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }
#endif
  // No other valid slot to fall back to (serial-flashed image, single-app
  // partition table, or ESP8266, which has no A/B slots at all). Restarting
  // here would land in the same image and, with the watchdog re-arming on
  // every boot, turn a failed self-test into a permanent reset loop.
  _emitEvent("status", "rollback-unavailable");
  if (_onError) _onError(F("rollback-unavailable"));
}

// ---------- mounting --------------------------------------------------------

void VectiOTAClass::begin(AsyncWebServer *server, const String &username, const String &password) {
  _server = server;
  if (username.length()) { _auth = OtaAuth::Basic; _user = username; _pass = password; }

  if (_hwId.length() == 0) {
#if defined(ESP32)
    uint64_t mac = ESP.getEfuseMac(); char buf[18];
    snprintf(buf, sizeof(buf), "%012llX", mac); _hwId = buf;
#else
    _hwId = String(ESP.getChipId(), HEX);
#endif
  }

  // Arm rollback watchdog. If the user calls commit() within the window,
  // we cancel; otherwise loop() will trigger rollback(). Only meaningful
  // while the bootloader is still waiting on a verdict for this image —
  // arming on an ordinary boot gives the watchdog nothing to revert to.
  if (_rollbackTimeoutMs > 0 && runningImagePendingVerify()) {
    _rollbackArmedAtMs = millis();
    _rollbackArmed = true;
  }

  // ---- /ota → UI ----
  // AsyncWebServer 3.x defaults to "BackwardCompatible" matching, where a
  // plain `on("/ota")` ALSO matches `/ota/info`, `/ota/upload`, ... and
  // intercepts them before the more specific handlers run. Use the explicit
  // exact() matcher to keep `/ota` confined to the literal `/ota` path.
  _server->on(AsyncURIMatcher::exact("/ota"), HTTP_GET, [this](AsyncWebServerRequest *req){
    if (!_authorize(req)) return req->requestAuthentication();
    sendGzippedUi(req, vecti::OTA_UI_HTML_GZ, vecti::OTA_UI_HTML_GZ_LEN, _auth != OtaAuth::None);
  });

  // ---- /ota/info → device JSON snapshot ----
  _server->on("/ota/info", HTTP_GET, [this](AsyncWebServerRequest *req){
    if (!_authorize(req)) return req->requestAuthentication();
    JsonDocument doc;
    doc["hwId"]       = _hwId;
    doc["fwVersion"]  = _fwVersion;
    doc["title"]      = _title;
    doc["brand"]      = _brandColor;
    doc["freeHeap"]   = ESP.getFreeHeap();
#if defined(ESP32)
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(nullptr);
    doc["currentSlot"] = running ? running->label : "?";
    doc["nextSlot"]    = next ? next->label : "?";
    doc["freeOta"]     = next ? next->size : 0;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &st);
    doc["slotState"]  = (st == ESP_OTA_IMG_PENDING_VERIFY) ? "pending" :
                        (st == ESP_OTA_IMG_VALID) ? "valid" :
                        (st == ESP_OTA_IMG_INVALID) ? "invalid" : "unknown";
#else
    doc["freeOta"] = ESP.getFreeSketchSpace();
#endif
    doc["updating"]   = _updating;
    doc["allowFw"]    = _allowFirmware;
    doc["allowFs"]    = _allowFilesystem;
    doc["allowPull"]  = _allowPull;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ---- /ota/events → Server-Sent Events for live progress ----
  // The stream carries partition labels, error strings and progress, so it
  // gets the same gate as every other endpoint. _authorize() can't be reused
  // here: the handler answers before any request callback of ours runs, so
  // the credentials go into the handler's own auth middleware instead.
  _events = new AsyncEventSource("/ota/events");
  _applyEventsAuth();
  _server->addHandler(_events);

  // ---- /ota/upload → multipart firmware/fs upload ----
  // The "completion" callback (3rd arg) fires after the body is fully
  // received; we send the final 200/4xx from there. The upload callback
  // (5th arg) streams bytes into Update.write() as they arrive.
  _server->on("/ota/upload", HTTP_POST,
    [this](AsyncWebServerRequest *req){
      // Completion handler — AsyncTCP task, runs after upload chunks finish.
      if (!_authorize(req)) return req->requestAuthentication();
      // A POST with no multipart body (Content-Length: 0, wrong content type)
      // never runs the chunk handler, so it owns none of the state below.
      // Answering from it would 200 + reboot on somebody else's transfer.
      if (_uploadOwner != req) {
        return _updating ? req->send(409, "text/plain", "busy")
                         : req->send(400, "text/plain", "no image received");
      }
      // _updating is cleared only by a successful Update.end(true), so a body
      // that stopped before final=true (malformed multipart, truncated part)
      // is a failure rather than a 200 + reboot into the old image.
      bool ok = _uploadStarted && !_updating && _uploadError.length() == 0;
      String reason = _uploadError.length() ? _uploadError : String(F("no image received"));
      int    status = ok ? 200 : (_uploadStatus ? _uploadStatus : 400);

      AsyncWebServerResponse *res = req->beginResponse(status, "text/plain", ok ? String("OK") : reason);
      res->addHeader("Connection","close");
      req->send(res);
      _emitEvent("status", ok ? String("complete") : reason);
      if (_onEnd) _onEnd(ok, ok ? String("complete") : reason);
      // ponytail: one flash at a time — the updater state is a single set of
      // members owned by _uploadOwner, and every non-owner already returned
      // above without touching it. Concurrent uploads would need per-request
      // state, i.e. a request-scoped allocation.
      _resetUploadState();
      if (ok) {
        bool doReboot = true;
        if (_onBeforeReboot) doReboot = _onBeforeReboot();
        if (doReboot) _scheduleReboot(150, false);
      }
    },
    [this](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final){
      // Upload chunk handler — AsyncTCP task, same task as the completion
      // handler above, so the shared _upload* members need no lock; they need
      // an owner, which is what _uploadOwner is.
      if (!_authorize(req)) return;
      if (index == 0) {
        if (_updating) {
          // A pull, or another client's upload, already owns the updater.
          // Touching Update — or _upload*, which belong to that transfer —
          // would interleave two images in one partition. Refuse silently;
          // the completion handler turns "not the owner + busy" into 409.
          _emitEvent("status", "busy");
          return;
        }
        _resetUploadState();
        _uploadOwner = req;
        if (!_rateLimitOk(req)) return _rejectUpload(429, F("rate-limited"));
        String modeArg = req->hasParam("mode") ? req->getParam("mode")->value() : String("firmware");
        _mode = (modeArg == "filesystem") ? OtaMode::Filesystem : OtaMode::Firmware;
        if (_mode == OtaMode::Firmware   && !_allowFirmware)   return _rejectUpload(403, F("fw-disabled"));
        if (_mode == OtaMode::Filesystem && !_allowFilesystem) return _rejectUpload(403, F("fs-disabled"));

        if (_signingKey.length()) {
          // The digest covers the whole body and can only be checked once the
          // last chunk lands, but the header has to be captured now — and a
          // missing one must stop the flash before a single byte is written.
          const AsyncWebHeader *sig = req->getHeader("X-Vecti-Signature");
          if (!sig) return _rejectUpload(400, F("sig-missing"));
          _sigExpected = sig->value();
          // Fail closed: if the HMAC engine won't start (malformed key, no
          // crypto backend) the image is refused, never accepted unchecked.
          if (!_sigBegin()) return _rejectUpload(400, F("sig-unavailable"));
        }

#if defined(ESP32)
        int updateCmd = (_mode == OtaMode::Filesystem) ? U_SPIFFS : U_FLASH;
        // contentLength() is the whole multipart envelope — boundaries and
        // part headers included, a couple hundred bytes more than the image —
        // so handing it to begin() rejects an image that would just fit. Let
        // the updater size itself against the partition instead.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateCmd)) {
#else
        int updateCmd = (_mode == OtaMode::Filesystem) ? U_FS : U_FLASH;
        // ponytail: ESP8266's updater has no UPDATE_SIZE_UNKNOWN — it needs a
        // real size up front to pick the target region — so it keeps living
        // with the ~250-byte multipart overshoot. Only an image that fills the
        // partition to within that margin is affected.
        size_t declared = req->contentLength();
        if (!Update.begin(declared ? declared
                                   : (uint32_t)((ESP.getFreeSketchSpace()-0x1000)&0xFFFFF000), updateCmd)) {
#endif
          return _rejectUpload(500, String("begin-failed:") + updErr());
        }
        _updating = true; _uploadStarted = true;
        _written = 0; _lastEmit = 0; _total = req->contentLength();

        // AsyncWebServer never delivers final=true when the client vanishes
        // mid-body, so without this the updater stays running and every later
        // upload fails with "already running" until the board is power-cycled.
        req->onDisconnect([this, req](){
          // AsyncTCP task. Only clean up if this request still owns the
          // updater — a normally-completed upload already reset it, and
          // clearing here would stomp whatever started next.
          if (_uploadOwner == req) { _resetUploadState(); _emitEvent("status", "aborted"); }
        });

        if (_onStart) _onStart(_mode);
        _emitEvent("status", String("start:") + contentTypeFromMode(_mode));
      }

      // A refused upload still has its remaining chunks delivered; drain them.
      //
      // Ownership, not just _updating: a SECOND concurrent upload never reaches
      // the index==0 branch that would claim ownership, so gating on the global
      // _updating alone let its chunks fall straight through into the first
      // upload's Update session and corrupt the image being written.
      if (_uploadOwner != req || !_updating || _uploadError.length()) return;

      if (len) {
        _sigUpdate(data, len);
        size_t w = Update.write(data, len);
        if (w != len) return _rejectUpload(500, String("write-short:") + updErr());
        _written += len;
        // The true image size is only known on the last chunk; until then
        // _total is the inflated envelope length and the bar can't reach 100%.
        if (final) _total = _written;
        if (_onProgress) _onProgress(_written, _total);
        // Throttle SSE emissions — every ~32 KB is plenty for a smooth bar.
        if (_written - _lastEmit >= 32768 || final) {
          _lastEmit = _written;
          char buf[96]; snprintf(buf, sizeof(buf),
            "{\"written\":%u,\"total\":%u,\"mode\":\"%s\"}",
            (unsigned)_written, (unsigned)_total,
            _mode==OtaMode::Filesystem?"filesystem":"firmware");
          _emitEvent("progress", buf);
        }
      }

      if (final) {
        // Verify before end(true): end() is what marks the new slot bootable,
        // so a bad signature has to abort the updater ahead of it.
        if (_signingKey.length() && !_sigFinish(_sigExpected)) {
          return _rejectUpload(400, F("sig-mismatch"));
        }
        if (!Update.end(true)) return _rejectUpload(500, String("end-failed:") + updErr());
        _updating = false;
      }
    });

  // ---- /ota/pull → queue a URL for the device to fetch ----
  // The body handler runs first (once per body chunk) and does all the work;
  // the request handler below only reports what it decided.
  _server->on("/ota/pull", HTTP_POST,
    [this](AsyncWebServerRequest *req){
      if (!_authorize(req)) return req->requestAuthentication();
      if (!_allowPull) return req->send(403, "text/plain", "pull disabled");
      if (!kPullSupported) return req->send(501, "text/plain", "pull unsupported on this target");
      String err = _pullError; _pullError = "";
      if (err.length()) {
        int code = (err == "busy") ? 409 : (err == "rate-limited") ? 429
                 : (err == "body-too-large") ? 413 : 400;
        return req->send(code, "text/plain", err);
      }
      // Read the latch, not _pullPending: loop() clears that flag the instant
      // it picks the job up, so a pull that was accepted a microsecond ago
      // would be reported back as 400 while the device is already flashing.
      bool queued = _pullAccepted; _pullAccepted = false;
      req->send(queued ? 202 : 400, "text/plain", queued ? "queued" : "missing body");
    },
    NULL,
    [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if (!kPullSupported || !_authorize(req) || !_allowPull) return;
      if (index == 0) {
        _pullError = ""; _pullBody = ""; _pullAccepted = false;
        // Pull erases and rewrites the whole OTA partition, exactly the flash
        // wear the upload limiter exists to bound — so it gets the same gate.
        // Emit as well as latch: an operator watching /ota/events in a second
        // tab is the documented way to see this, and only the HTTP response
        // reaches the client that tripped the limiter.
        if (!_rateLimitOk(req))          { _pullError = F("rate-limited");
                                           _emitEvent("status", _pullError); return; }
        if (_pullPending || _updating)   { _pullError = F("busy");         return; }
      }
      if (_pullError.length()) return;
      if (total > kPullBodyMax || _pullBody.length() + len > kPullBodyMax) {
        _pullError = F("body-too-large"); _pullBody = ""; return;
      }
      if (!_pullBody.concat((const char*)data, len)) {
        _pullError = F("oom"); _pullBody = ""; return;
      }
      if (index + len != total) return;

      JsonDocument doc;
      if (deserializeJson(doc, _pullBody) != DeserializationError::Ok) {
        _pullError = F("bad-json");
      } else {
        String url = doc["url"] | "";
        String modeS = doc["mode"] | "firmware";
        if (!url.length()) {
          _pullError = F("missing-url");
        } else {
          _pullMode = (modeS == "filesystem") ? OtaMode::Filesystem : OtaMode::Firmware;
          _pullUrl  = url;
          // _pullAccepted is this request's own verdict (AsyncTCP task only);
          // _pullPending is the handoff to loop(), published last because
          // loop() only touches _pullUrl once it is set — and we only get here
          // with it clear, so the two tasks never hold the same String buffer.
          _pullAccepted = true;
          _pullPending  = true;
        }
      }
      _pullBody = "";
    });

  // ---- /ota/commit → mark slot valid (called after self-test passes) ----
  _server->on("/ota/commit", HTTP_POST, [this](AsyncWebServerRequest *req){
    if (!_authorize(req)) return req->requestAuthentication();
    commit();
    req->send(200, "text/plain", "committed");
  });

  // ---- /ota/rollback → revert to previous slot ----
  _server->on("/ota/rollback", HTTP_POST, [this](AsyncWebServerRequest *req){
    if (!_authorize(req)) return req->requestAuthentication();
    if (!_rateLimitOk(req)) {
      _emitEvent("status", F("rate-limited"));
      return req->send(429, "text/plain", "rate-limited");
    }
    // Decide here, not inside rollback() on the loop task: an SSE status event
    // is the client's only other feedback channel and it is closed in token
    // mode, so answering 200 would leave the operator watching nothing.
#if defined(ESP32)
    if (!esp_ota_check_rollback_is_possible())
      return req->send(409, "text/plain", "rollback-unavailable");
#else
    return req->send(501, "text/plain", "rollback unsupported on this target");
#endif
    AsyncWebServerResponse *res = req->beginResponse(200, "text/plain", "rolling back");
    res->addHeader("Connection","close");
    req->send(res);
    _scheduleReboot(200, true);
  });
}

// ---------- loop tasks ------------------------------------------------------

void VectiOTAClass::loop() {
  // Deferred reboot — the handler that asked for it runs on the AsyncTCP task
  // and cannot wait for its own response to be written.
  if (_rebootPending && (int32_t)(millis() - _rebootAtMs) >= 0) {
    _rebootPending = false;
    if (_rebootIsRollback) rollback();
    else                   ESP.restart();
  }

  // Rollback watchdog — fire if the new firmware didn't commit in time.
  if (_rollbackArmed && _rollbackTimeoutMs > 0 && !_committed) {
    if ((millis() - _rollbackArmedAtMs) > _rollbackTimeoutMs) {
      _rollbackArmed = false;
      _emitEvent("status","rollback-watchdog");
      rollback();
    }
  }
  _processPullQueue();
}

void VectiOTAClass::_processPullQueue() {
  if (!_pullPending) return;
  String url = _pullUrl; OtaMode mode = _pullMode;
  // Clear the buffer before the flag: the flag is what tells the TCP task the
  // slot is free again, so it must be the last write.
  _pullUrl = "";
  _pullPending = false;
  _executePull(url, mode);
}

bool VectiOTAClass::_executePull(const String &url, OtaMode mode) {
#if defined(ESP32)
  WiFiClient httpClient;
  WiFiClientSecure tlsClient;
  WiFiClient *cli = &httpClient;
  if (url.startsWith("https://")) {
    if (_pullCaCert)          tlsClient.setCACert(_pullCaCert);
    else if (_pullInsecure)   tlsClient.setInsecure();
    else {
      // WiFiClientSecure verifies nothing until it is told what to trust, so
      // an unpinned pull would flash whatever answers the DNS query. Refuse
      // rather than hand the boot image to anyone who can spoof a hostname.
      _emitEvent("status", "pull-tls-unpinned");
      if (_onError) _onError(F("pull-tls-unpinned"));
      return false;
    }
    cli = &tlsClient;
  }

  HTTPClient http;
  // Release assets and pre-signed object-store URLs answer 30x; without this
  // the most common way to host firmware simply doesn't work.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(kPullIdleMs);
  if (!http.begin(*cli, url)) { _emitEvent("status","pull-begin-failed"); return false; }
  int code = http.GET();
  if (code != 200) { _emitEvent("status", String("pull-http-")+code); http.end(); return false; }
  int size = http.getSize();
  if (size <= 0) {
    // getSize() is -1 for a chunked or length-less response. Two reasons to
    // refuse rather than stream it: there is no way to tell "server finished"
    // from "server stalled", so a truncated body would be flashed and booted;
    // and getStreamPtr() hands back the raw chunk framing (writeToStream() is
    // what de-chunks), so the bytes would be garbage anyway. Unlike /ota/upload
    // there is no signature to catch it. Serve firmware with a Content-Length.
    _emitEvent("status", "pull-no-length");
    if (_onError) _onError(F("pull-no-length"));
    http.end(); return false;
  }
  int cmd = (mode == OtaMode::Filesystem) ? U_SPIFFS : U_FLASH;
  if (!Update.begin((size_t)size, cmd)) {
    _emitEvent("status", String("pull-update-begin:")+updErr()); http.end(); return false;
  }
  _updating = true; _written = 0; _total = size; _lastEmit = 0;
  if (_onStart) _onStart(mode);

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  uint32_t deadline = millis() + kPullTotalMs;
  uint32_t lastByteMs = millis();
  bool complete = false;

  // Every exit below is bounded. A keep-alive socket stays "connected" after
  // the body ends, so http.connected() alone can never end this loop; `size`
  // counts down to zero and is the only path that sets complete.
  while (true) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = std::min(avail, sizeof(buf));
      if (want > (size_t)size) want = (size_t)size;
      size_t n = stream->readBytes(buf, want);
      if (Update.write(buf, n) != n) { _emitEvent("status","pull-write-short"); break; }
      _written += n;
      lastByteMs = millis();
      size -= n;
      if (_onProgress) _onProgress(_written, _total);
      if (_written - _lastEmit >= 32768) {
        _lastEmit = _written;
        char m[96]; snprintf(m, sizeof(m), "{\"written\":%u,\"total\":%u,\"mode\":\"pull\"}",
          (unsigned)_written,(unsigned)_total);
        _emitEvent("progress", m);
      }
      if (size == 0) { complete = true; break; }   // declared body fully written
      continue;
    }
    // Short of the declared length: never "close enough". A quiet socket is a
    // truncated download, not an EOF, and end(true) would mark a half-written
    // slot bootable.
    if (!http.connected() || millis() - lastByteMs > kPullIdleMs) {
      _emitEvent("status", http.connected() ? "pull-stalled" : "pull-truncated");
      break;
    }
    if ((int32_t)(millis() - deadline) >= 0) { _emitEvent("status","pull-timeout"); break; }
    delay(1);
  }

  bool ok = false;
  if (complete) ok = Update.end(true);
  else          updAbort();
  // The updater is closed either way, so release the interlock — this is the
  // only upload-related member the pull path owns.
  _updating = false;
  http.end();
  _emitEvent("status", ok ? "pull-complete" : (String("pull-end:")+updErr()));
  if (_onEnd) _onEnd(ok, ok ? String("pull-complete") : updErr());
  // Deliberately NOT _resetUploadState(): that also clears
  // _uploadOwner/_uploadStarted/_uploadStatus/_uploadError, which the header
  // declares AsyncTCP-task-only. The callbacks just above are a multi-ms
  // window in which an upload can legitimately claim the updater, and reaching
  // into its state from the loop task would abort its flash and free a String
  // it is reading. Nothing to reset here either: _written/_total/_lastEmit are
  // re-initialised by whichever transfer starts next, and the updater is
  // already closed by the end(true)/updAbort() above.
  if (ok) _scheduleReboot(200, false);
  return ok;
#else
  (void)url; (void)mode;
  _emitEvent("status", "pull-unsupported");
  if (_onError) _onError(F("pull-unsupported"));
  return false;
#endif
}

} // namespace vecti

vecti::VectiOTAClass VectiOTA;
