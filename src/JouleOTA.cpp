// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------

// JouleOTA implementation. See JouleOTA.h for the rationale + public API.
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

#include "JouleOTA.h"
#include "JouleOTA_ui.h"
#include "JouleOTA_ui_gz.h"
#include <ArduinoJson.h>

// Serve pre-compressed UI with Content-Encoding: gzip — browsers inflate
// transparently and the ~10 KB SPA drops to ~3.7 KB on the wire, which is
// what makes it survive weak-RSSI links where the uncompressed response
// stalls mid-stream.
static void sendGzippedUi(AsyncWebServerRequest *req, const uint8_t *gz, size_t len) {
  AsyncWebServerResponse *res = req->beginResponse(200, "text/html; charset=utf-8", gz, len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "public, max-age=3600");
  req->send(res);
}

#if defined(ESP32)
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#elif defined(ESP8266)
  #include <ESP8266HTTPClient.h>
  #include <WiFiClientSecureBearSSL.h>
  #include <LittleFS.h>
#endif

namespace joule {

JouleOTAClass::JouleOTAClass() = default;

// ---------- helpers ---------------------------------------------------------

static String contentTypeFromMode(OtaMode m) {
  return (m == OtaMode::Filesystem) ? F("filesystem") : F("firmware");
}

// HMAC-SHA256 over an arbitrary byte range, returning lowercase hex. Used to
// verify the X-Joule-Signature header against the body. Implemented with
// mbedtls (already linked in by arduino-esp32) so we don't pull in a second
// crypto lib.
static String hmacSha256Hex(const String &hexKey, const uint8_t *data, size_t len) {
#if defined(ESP32)
  uint8_t key[64]; size_t keyLen = 0;
  for (size_t i = 0; i + 1 < hexKey.length() && keyLen < sizeof(key); i += 2) {
    auto h = [](char c){ return c<='9'?c-'0':(c|0x20)-'a'+10; };
    key[keyLen++] = (uint8_t)((h(hexKey[i])<<4) | h(hexKey[i+1]));
  }
  uint8_t mac[32];
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, data, len);
  mbedtls_md_hmac_finish(&ctx, mac);
  mbedtls_md_free(&ctx);
  static const char hex[] = "0123456789abcdef";
  char out[65]; for (int i=0;i<32;i++){out[i*2]=hex[mac[i]>>4];out[i*2+1]=hex[mac[i]&0xF];}
  out[64]=0; return String(out);
#else
  return String();
#endif
}

// Replace template tokens in the embedded HTML at serve time so the UI can
// reflect setTitle() / setBrandColor() without rebuilding the firmware.
static String renderHtml(const String &title, const String &brand) {
  String s = FPSTR(OTA_UI_HTML);
  s.replace("__TITLE__", title);
  s.replace("__BRAND__", brand);
  return s;
}

bool JouleOTAClass::_verifySignature(const uint8_t *body, size_t len, const String &hexSig) const {
  if (_signingKey.length() == 0) return true;
  if (hexSig.length() != 64) return false;
  String calc = hmacSha256Hex(_signingKey, body, len);
  // constant-time compare so a remote attacker can't byte-walk the digest
  // off the timing curve.
  if (calc.length() != hexSig.length()) return false;
  uint8_t diff = 0;
  for (size_t i=0;i<calc.length();i++) diff |= (uint8_t)(calc[i]) ^ (uint8_t)(hexSig[i]);
  return diff == 0;
}

bool JouleOTAClass::_authorize(AsyncWebServerRequest *req) const {
  if (_auth == OtaAuth::None) return true;
  if (_auth == OtaAuth::Basic) {
    if (_user.length()==0) return true;
    return req->authenticate(_user.c_str(), _pass.c_str());
  }
  if (_auth == OtaAuth::Token) {
    if (!req->hasHeader("X-Joule-Token")) return false;
    return req->getHeader("X-Joule-Token")->value() == _token;
  }
  return false;
}

bool JouleOTAClass::_rateLimitOk(AsyncWebServerRequest *req) {
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

void JouleOTAClass::_emitEvent(const String &type, const String &payload) {
  if (_events) _events->send(payload.c_str(), type.c_str(), millis());
}

void JouleOTAClass::_resetUploadState() {
  _updating = false; _written = 0; _total = 0;
}

// ---------- public API ------------------------------------------------------

void JouleOTAClass::setAuth(OtaAuth mode, const String &userOrToken, const String &password) {
  _auth = mode;
  if (mode == OtaAuth::Basic) { _user = userOrToken; _pass = password; _token = ""; }
  else if (mode == OtaAuth::Token) { _token = userOrToken; _user = _pass = ""; }
  else { _user = _pass = _token = ""; }
}

void JouleOTAClass::commit() {
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

void JouleOTAClass::rollback() {
#if defined(ESP32)
  if (esp_ota_check_rollback_is_possible()) {
    esp_ota_mark_app_invalid_rollback_and_reboot();
  } else {
    ESP.restart();
  }
#else
  ESP.restart();
#endif
}

// ---------- mounting --------------------------------------------------------

void JouleOTAClass::begin(AsyncWebServer *server, const String &username, const String &password) {
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
  // we cancel; otherwise loop() will trigger rollback().
  if (_rollbackTimeoutMs > 0) {
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
    sendGzippedUi(req, joule::OTA_UI_HTML_GZ, joule::OTA_UI_HTML_GZ_LEN);
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
  _events = new AsyncEventSource("/ota/events");
  _server->addHandler(_events);

  // ---- /ota/upload → multipart firmware/fs upload ----
  // The "completion" callback (3rd arg) fires after the body is fully
  // received; we send the final 200/4xx from there. The upload callback
  // (5th arg) streams bytes into Update.write() as they arrive.
  _server->on("/ota/upload", HTTP_POST,
    [this](AsyncWebServerRequest *req){
      // Completion handler — runs after upload chunks finish.
      if (!_authorize(req)) return req->requestAuthentication();
      bool ok = !Update.hasError();
      AsyncWebServerResponse *res = req->beginResponse(ok?200:500, "text/plain",
        ok ? "OK" : Update.errorString());
      res->addHeader("Connection","close");
      req->send(res);
      _emitEvent("status", ok ? "complete" : Update.errorString());
      if (_onEnd) _onEnd(ok, ok ? String("complete") : String(Update.errorString()));
      _resetUploadState();
      if (ok) {
        bool doReboot = true;
        if (_onBeforeReboot) doReboot = _onBeforeReboot();
        if (doReboot) { delay(150); ESP.restart(); }
      }
    },
    [this](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final){
      // Upload chunk handler.
      if (!_authorize(req)) return;
      if (index == 0) {
        if (!_rateLimitOk(req)) { _emitEvent("status","rate-limited"); return; }
        String modeArg = req->hasParam("mode") ? req->getParam("mode")->value() : String("firmware");
        _mode = (modeArg == "filesystem") ? OtaMode::Filesystem : OtaMode::Firmware;
        if (_mode == OtaMode::Firmware   && !_allowFirmware)   { _emitEvent("status","fw-disabled");   return; }
        if (_mode == OtaMode::Filesystem && !_allowFilesystem) { _emitEvent("status","fs-disabled");   return; }

        size_t total = req->contentLength();
#if defined(ESP32)
        int updateCmd = (_mode == OtaMode::Filesystem) ? U_SPIFFS : U_FLASH;
        if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, updateCmd)) {
#else
        int updateCmd = (_mode == OtaMode::Filesystem) ? U_FS : U_FLASH;
        if (!Update.begin(total ? total : (uint32_t)((ESP.getFreeSketchSpace()-0x1000)&0xFFFFF000), updateCmd)) {
#endif
          _emitEvent("status", String("begin-failed:") + Update.errorString());
          if (_onError) _onError(String(Update.errorString()));
          return;
        }
        _updating = true; _written = 0; _total = total;
        if (_onStart) _onStart(_mode);
        _emitEvent("status", String("start:") + contentTypeFromMode(_mode));
      }

      if (len) {
        size_t w = Update.write(data, len);
        if (w != len) {
          _emitEvent("status", String("write-short:") + Update.errorString());
          if (_onError) _onError(String(Update.errorString()));
          return;
        }
        _written += len;
        if (_onProgress) _onProgress(_written, _total);
        // Throttle SSE emissions — every ~32 KB is plenty for a smooth bar.
        static size_t lastEmit = 0;
        if (_written - lastEmit >= 32768 || final) {
          lastEmit = _written;
          char buf[96]; snprintf(buf, sizeof(buf),
            "{\"written\":%u,\"total\":%u,\"mode\":\"%s\"}",
            (unsigned)_written, (unsigned)_total,
            _mode==OtaMode::Filesystem?"filesystem":"firmware");
          _emitEvent("progress", buf);
        }
      }

      if (final) {
        if (!Update.end(true)) {
          _emitEvent("status", String("end-failed:") + Update.errorString());
          if (_onError) _onError(String(Update.errorString()));
        }
      }
    });

  // ---- /ota/pull → queue a URL for the device to fetch ----
  _server->on("/ota/pull", HTTP_POST,
    [this](AsyncWebServerRequest *req){
      // Body handler attaches; final response sent from body handler when JSON parsed.
      if (!_authorize(req)) return req->requestAuthentication();
      if (!_allowPull) return req->send(403, "text/plain", "pull disabled");
      // If body handler already populated _pullPending we're good; otherwise
      // the client probably sent an empty POST.
      req->send(_pullPending ? 202 : 400, "text/plain", _pullPending ? "queued" : "missing body");
    },
    NULL,
    [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if (!_authorize(req) || !_allowPull) return;
      static String buf;
      if (index == 0) buf = "";
      buf.concat((const char*)data, len);
      if (index + len == total) {
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
          _pullUrl = doc["url"] | "";
          String modeS = doc["mode"] | "firmware";
          _pullMode = (modeS == "filesystem") ? OtaMode::Filesystem : OtaMode::Firmware;
          if (_pullUrl.length()) _pullPending = true;
        }
      }
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
    AsyncWebServerResponse *res = req->beginResponse(200, "text/plain", "rolling back");
    res->addHeader("Connection","close");
    req->send(res);
    delay(200);
    rollback();
  });
}

// ---------- loop tasks ------------------------------------------------------

void JouleOTAClass::loop() {
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

void JouleOTAClass::_processPullQueue() {
  if (!_pullPending) return;
  String url = _pullUrl; OtaMode mode = _pullMode;
  _pullPending = false; _pullUrl = "";
  _executePull(url, mode);
}

bool JouleOTAClass::_executePull(const String &url, OtaMode mode) {
#if defined(ESP32)
  WiFiClient *cli = nullptr;
  WiFiClient httpClient;
  WiFiClientSecure tlsClient;
  if (url.startsWith("https://")) { tlsClient.setInsecure(); cli = &tlsClient; }
  else cli = &httpClient;

  HTTPClient http;
  if (!http.begin(*cli, url)) { _emitEvent("status","pull-begin-failed"); return false; }
  int code = http.GET();
  if (code != 200) { _emitEvent("status", String("pull-http-")+code); http.end(); return false; }
  int size = http.getSize();
  int cmd = (mode == OtaMode::Filesystem) ? U_SPIFFS : U_FLASH;
  if (!Update.begin(size > 0 ? (size_t)size : (size_t)UPDATE_SIZE_UNKNOWN, cmd)) {
    _emitEvent("status", String("pull-update-begin:")+Update.errorString()); http.end(); return false;
  }
  _updating = true; _written = 0; _total = size>0?size:0;
  if (_onStart) _onStart(mode);

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024]; size_t lastEmit = 0;
  while (http.connected() && (size > 0 || size == -1)) {
    size_t avail = stream->available();
    if (avail) {
      size_t n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
      if (Update.write(buf, n) != n) { _emitEvent("status","pull-write-short"); break; }
      _written += n;
      if (size > 0) size -= n;
      if (_onProgress) _onProgress(_written, _total);
      if (_written - lastEmit >= 32768) {
        lastEmit = _written;
        char m[96]; snprintf(m, sizeof(m), "{\"written\":%u,\"total\":%u,\"mode\":\"pull\"}",
          (unsigned)_written,(unsigned)_total);
        _emitEvent("progress", m);
      }
    } else delay(1);
    if (size == 0) break;
  }
  bool ok = Update.end(true);
  http.end();
  _emitEvent("status", ok ? "pull-complete" : (String("pull-end:")+Update.errorString()));
  if (_onEnd) _onEnd(ok, ok ? "pull-complete" : Update.errorString());
  _resetUploadState();
  if (ok) { delay(200); ESP.restart(); }
  return ok;
#else
  (void)url; (void)mode; return false;
#endif
}

} // namespace joule

joule::JouleOTAClass JouleOTA;
