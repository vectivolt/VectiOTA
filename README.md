# JouleOTA

> Async over-the-air firmware updater for ESP32 and ESP8266 — drag-drop in
> a browser, pull from a URL, sign with HMAC-SHA256, A/B-rollback if the
> new image goes bad. MIT-licensed, mobile-friendly, 5 KB on the wire.

![JouleOTA UI](docs/screenshots/ota-desktop.png)

**Author:** [Chinmoy Bhuyan](mailto:dikibhuyan@gmail.com) · **License:** MIT
· **Targets:** ESP32 (S2 / S3 / C3 / classic), ESP8266

---

## Features

| | |
|---|---|
| 🖱  **Drag-drop UI** | Polished single-page updater with SVG progress ring, live byte/throughput counters, mode tabs |
| ☁  **Pull from URL** | Tell the device to fetch a firmware from any HTTP/HTTPS URL — perfect for fleet rollouts from a build pipeline |
| 🔐 **Signed firmware (optional)** | HMAC-SHA256 over the full body with a constant-time hex compare. Disabled by default; enable with one call |
| ↺  **A/B rollback** | If the new firmware never calls `commit()` within your timeout, the bootloader picks the previous slot on the next reset |
| 📁 **Firmware + filesystem** | Switch modes from the same UI to flash either the app partition or SPIFFS / LittleFS |
| 📡 **Live progress via SSE** | Every connected browser tab updates in real-time, not just the one that started the upload |
| ⏱  **Rate limiting** | Per-IP minimum gap (default 5 s) so a brute-force auth attempt can't burn the flash erase cycle counter |
| 🪪 **HW ID + version** | Free-form identity strings exposed on the page and in `/ota/info` JSON |
| 🔒 **HTTP Basic + token auth** | Pick `OtaAuth::Basic` for browser flows, `OtaAuth::Token` for headless CI |
| 🎨 **Theme aware** | Dark / light / auto; user choice persists; brand colour configurable |
| 📱 **Mobile-first** | 44 px touch targets, viewport-fit safe-area, glass-morphism panels |
| 🪶 **5 KB on the wire** | Pre-gzipped UI served with `Content-Encoding: gzip` |

---

## Quick start

```cpp
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleOTA.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(250);

  JouleOTA.setID(WiFi.macAddress());
  JouleOTA.setFWVersion("1.0.0");
  JouleOTA.begin(&server, "admin", "joule");
  server.begin();

  JouleOTA.commit();           // mark this firmware as known-good
}

void loop() { JouleOTA.loop(); }
```

Open `http://<device-ip>/ota` and drop a `firmware.bin`. Done.

---

## API reference

### Lifecycle

```cpp
void begin(AsyncWebServer *server,
           const String &username = "",
           const String &password = "");
void loop();          // call from loop() for pull-mode polling + rollback timing
```

`begin()` mounts every endpoint described in the table below. If
`username` is non-empty the library defaults to HTTP Basic auth using
those credentials; pass `""` to disable auth.

### Identity & branding

```cpp
void setID         (const String &id);          // shown in UI + /ota/info
void setFWVersion  (const String &ver);
void setTitle      (const String &title);       // page title
void setBrandColor (const String &cssColor);    // accent colour
```

### Authentication

```cpp
enum class OtaAuth { None, Basic, Token };

void setAuth(OtaAuth mode, const String &userOrToken, const String &password = "");
void clearAuth();
```

* `OtaAuth::Basic` → standard browser-prompted Basic auth.
* `OtaAuth::Token` → expects header `X-Joule-Token: <token>` on every
  request. Use this for CI flows where you don't want a browser dialog.

### Signed firmware (recommended for production)

```cpp
void setSigningKey(const String &hexKey);       // empty = disable (default)
```

If a key is set, each upload must include header
`X-Joule-Signature: <hex>` where `hex` is the HMAC-SHA256 of the full
body computed with the key. The verifier uses a constant-time compare to
defeat timing oracles. Recommended key length: 32 bytes (64 hex chars).

### Policy

```cpp
void setRateLimitMs       (uint32_t ms);   // min gap between upload starts per IP
void allowFirmwareUpdates  (bool on);      // default true
void allowFilesystemUpdates(bool on);      // default true
void allowPullMode         (bool on);      // default true
```

### Rollback

```cpp
void setRollbackTimeoutMs(uint32_t ms);   // 0 = disabled (default)
void commit();                            // call from setup() after self-test
void rollback();                          // force immediate revert + reboot
```

Recommended flow:

```cpp
void setup() {
  …
  JouleOTA.setRollbackTimeoutMs(30000);   // 30 s grace period
  JouleOTA.begin(&server, "admin", "pass");

  if (runSelfTest()) JouleOTA.commit();
  // else: do nothing → bootloader will revert on next reset
}
```

### Callbacks

```cpp
using OtaStartCb     = std::function<void(OtaMode mode)>;
using OtaProgressCb  = std::function<void(size_t current, size_t total)>;
using OtaEndCb       = std::function<void(bool success, const String &msg)>;
using OtaErrorCb     = std::function<void(const String &reason)>;
using OtaRebootCb    = std::function<bool()>;   // return false to suppress reboot

void onStart       (OtaStartCb     cb);
void onProgress    (OtaProgressCb  cb);
void onEnd         (OtaEndCb       cb);
void onError       (OtaErrorCb     cb);
void onBeforeReboot(OtaRebootCb    cb);
```

Use `onBeforeReboot` to do a graceful shutdown (close TLS sockets, write
state to NVS, …) before the device restarts after a successful flash.

### Status accessors

```cpp
bool    isUpdating()    const;
size_t  bytesWritten()  const;
size_t  totalBytes()    const;
uint8_t progressPct()   const;
```

---

## HTTP endpoints

| Path | Method | Auth | Description |
|---|---|---|---|
| `/ota`        | GET  | yes | The drag-drop SPA |
| `/ota/info`   | GET  | yes | JSON snapshot of device + partition state |
| `/ota/upload` | POST | yes | Multipart firmware/filesystem upload (`?mode=firmware\|filesystem`) |
| `/ota/pull`   | POST | yes | `{"url":"…","mode":"firmware"}` — device fetches over HTTP/HTTPS |
| `/ota/events` | SSE  | yes | Live progress + status events |
| `/ota/commit` | POST | yes | Mark current slot valid (cancels pending rollback) |
| `/ota/rollback` | POST | yes | Revert to previous slot and reboot |

### `/ota/info` payload

```json
{
  "hwId":        "D0:CF:13:73:0A:B8",
  "fwVersion":   "1.0.0+demo",
  "title":       "JouleSuite OTA",
  "freeHeap":    250248,
  "currentSlot": "app0",
  "nextSlot":    "app1",
  "freeOta":     3342336,
  "slotState":   "valid",
  "updating":    false,
  "allowFw":     true,
  "allowFs":     true,
  "allowPull":   true
}
```

### `/ota/events` event stream

```
event: progress
data: {"written":131072,"total":1115632,"mode":"firmware"}

event: status
data: complete
```

`progress` fires every ~32 KB; `status` fires for lifecycle transitions
(`start:firmware`, `complete`, `rate-limited`, `pull-http-404`, …).

---

## Pull-from-URL flow

```bash
curl -u admin:joule -X POST http://device.local/ota/pull \
  -H "Content-Type: application/json" \
  -d '{"url":"https://builds.example.com/v1.2.3/firmware.bin","mode":"firmware"}'
```

The device queues the URL, downloads it on the next `loop()` tick
(non-blocking — the HTTP request returns `202 Accepted` immediately),
flashes, and reboots. HTTPS URLs use `WiFiClientSecure::setInsecure()`
by default — pin a fingerprint in your sketch if you need TLS-pinning.

---

## Signed-firmware workflow

1. Generate a 32-byte key once and store it in your build secrets:

   ```bash
   openssl rand -hex 32   # 64-char hex
   ```

2. In firmware:

   ```cpp
   JouleOTA.setSigningKey("a9f1…");      // the hex string
   ```

3. In your CI, sign each release before upload:

   ```bash
   SIG=$(openssl dgst -sha256 -mac HMAC -macopt hexkey:a9f1… -hex firmware.bin | awk '{print $2}')
   curl -u admin:joule -X POST http://device/ota/upload?mode=firmware \
        -H "X-Joule-Signature: $SIG" \
        -F update=@firmware.bin
   ```

Unsigned uploads will be rejected with `400 Bad Request`.

---

## Rollback explained

ESP32's bootloader supports **two app slots**: `app0` and `app1`. JouleOTA
writes the new image into whichever slot is *not* running, then flags the
new slot as `PENDING_VERIFY`. On the next reset the bootloader runs the
new firmware once. If your sketch calls `JouleOTA.commit()` before the
rollback timeout expires, the slot is marked `VALID` and stays the active
slot. If it doesn't (because the firmware crashed, hung, or failed
self-test) the bootloader picks the previous `VALID` slot on the next
reset and you're back to a known-good state.

```cpp
void setup() {
  // … bring up Wi-Fi …
  JouleOTA.setRollbackTimeoutMs(30000);   // 30 s grace
  JouleOTA.begin(&server, "admin", "pw");

  // run smoke-tests: peripherals reachable, NTP synced, etc.
  if (smokeTestPasses()) {
    JouleOTA.commit();
  } else {
    JouleSerial.err("self-test failed — letting bootloader roll back");
  }
}
```

For a manual rollback (e.g. operator-triggered from the UI):

```cpp
JouleOTA.rollback();   // marks current invalid + reboots
```

---

## Authentication patterns

### Browser → HTTP Basic

```cpp
JouleOTA.begin(&server, "admin", "strong-password");
```

The browser prompts on first visit; credentials stick for the session.

### CI / scripts → token header

```cpp
JouleOTA.setAuth(joule::OtaAuth::Token, "long-random-token");
```

```bash
curl -X POST http://device/ota/upload?mode=firmware \
     -H "X-Joule-Token: long-random-token" \
     -F update=@firmware.bin
```

### Local-only / behind VPN → no auth

```cpp
JouleOTA.begin(&server);              // pass empty strings
```

---

## UI walkthrough

| Section | What you see |
|---|---|
| **Header** | Device title + theme toggle (◐) |
| **Device card** | HWID, FW version, current slot (`app0`/`app1`), next slot, free OTA bytes, free heap |
| **Mode tabs** | 🔧 Firmware · 📁 Filesystem · ☁ Pull URL |
| **Drop zone** | Drag a `.bin` (or `.bin.gz`) anywhere on the card, or click to browse |
| **Progress ring** | SVG arc fills with a brand-gradient as bytes upload; live %/MB/throughput |
| **Log pane** | Per-event log: file name + size, percent ticks, status messages, errors |
| **Pull-URL tab** | URL input + "Pull & flash" button (queues a fetch on the device) |
| **Maintenance card** | ✓ Commit current · ↺ Rollback (red, confirmation prompt) |

Mobile (390 px wide):

![JouleOTA mobile](docs/screenshots/ota-mobile.png)

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `400 Bad Request` on upload | Signed-firmware mode is on but signature header missing/wrong | Set the header or temporarily clear the key |
| `rate-limited` status event | Too many upload starts from the same IP in the last 5 s | Increase `setRateLimitMs(0)` or wait |
| `begin-failed:...esp_partition_find_first` | OTA partition layout missing | Use a `default_8MB.csv` (or larger) partition CSV |
| Upload completes, device reboots, then reverts every 30 s | Self-test isn't calling `commit()` after success | Add `JouleOTA.commit()` at the end of `setup()` |
| UI loads but progress stays at 0% | The browser tab uploading is fine; this tab is observing via SSE and the device is busy | Just wait — progress will sync up |
| `IncompleteRead` on weak Wi-Fi | TCP retransmits failing | Library already disables modem sleep + maxes TX power. Move the device closer or use a directional antenna |

---

## Comparison with build-it-yourself

| Concern | Roll-your-own | JouleOTA |
|---|---|---|
| Drag-drop UI | Write & maintain HTML | Included, 5 KB gz |
| Pull-from-URL | Bespoke HTTP client + state machine | One POST `/ota/pull` |
| Signature check | Hook into mbedtls manually | `setSigningKey()` |
| A/B rollback | Read `esp_ota_*` APIs by hand | `setRollbackTimeoutMs()` + `commit()` |
| Rate limiting | Add yourself | Built-in per-IP table |
| Live multi-client progress | Build an SSE channel | Included on `/ota/events` |
| Theme + branding | Custom CSS | `setTitle()`, `setBrandColor()` |

---

## Dependencies

* `ESP32Async/ESPAsyncWebServer @ ^3.7.0`
* `ESP32Async/AsyncTCP @ ^3.4.0`
* `bblanchon/ArduinoJson @ ^7.4.0`
* arduino-esp32 core 3.x (for `AsyncURIMatcher::exact()`)

---

## License

MIT — see [LICENSE](LICENSE).

---

<sub>**Author:** Chinmoy Bhuyan · **Email:** dikibhuyan@gmail.com · **(c)** 2026 — MIT</sub>
