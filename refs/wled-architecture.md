# WLED Architecture Reference

## Repository & Release Status

| Property | Value |
|---|---|
| Repo | https://github.com/wled/WLED (originally Aircoookie/WLED) |
| Latest Stable | **v16.0.1** (2026-07-07) |
| Version Code | `2607201` (YYMMDDB) |
| Default Branch | `main` |
| Platform | ESP-IDF 5.3.4 / arduino-esp32 v3.1.10 (V5); also V4 builds |

## Source File Map

| File | Purpose | Lines |
|---|---|---|
| `wled00/wled.h` | Global declarations, feature flags, includes | ~967 |
| `wled00/wled.cpp` | `WLED::setup()` and `WLED::loop()` — main orchestration | ~700 |
| `wled00/wled_main.cpp` | Arduino `setup()`/`loop()` → delegates to `WLED::instance()` | 24 |
| **`wled00/wled_serial.cpp`** | **Serial protocol handler (Adalight, TPM2, JSON, Improv)** | **202** |
| **`wled00/json.cpp`** | **JSON API: `deserializeState()`, `serializeState()`, `serializeInfo()`** | **~1400** |
| `wled00/udp.cpp` | UDP sync, realtime protocols, ESP-NOW | 987 |
| `wled00/e131.cpp` | E1.31/sACN, Art-Net, DDP handlers | 584 |
| `wled00/improv.cpp` | Improv Serial WiFi provisioning | 275 |
| `wled00/wled_server.cpp` | HTTP server, routes `/json/*` endpoints | varies |
| `wled00/set.cpp` | HTTP API handler (`handleSet()`) | varies |
| `wled00/cfg.cpp` | Configuration persistence (JSON to/from LittleFS) | varies |
| `wled00/const.h` | Constants, pin definitions, platform detection | 795 |
| `wled00/fcn_declare.h` | Forward declarations for all functions | varies |
| `wled00/FX.cpp` / `FX.h` | Effects engine | large |
| `wled00/bus_manager.cpp/.h` | LED bus abstraction (NeoPixelBus wrapper) | varies |
| `wled00/pin_manager.cpp/.h` | Pin allocation tracking | varies |

## Serial Implementation — `wled_serial.cpp`

### Entry Point

`handleSerial()` is called every main loop iteration. Gated by two globals:

```cpp
// wled.h
WLED_GLOBAL bool serialCanRX _INIT(false);
WLED_GLOBAL bool serialCanTX _INIT(false);
```

Set during `WLED::setup()` based on pin allocation:

```cpp
// wled.cpp
serialCanRX = !PinManager::isPinAllocated(hardwareRX);
serialCanTX = !PinManager::isPinAllocated(hardwareTX) || PinManager::getPinOwner(hardwareTX) == PinOwner::DebugOut;
```

**Key insight**: If GPIO3 (RX) is allocated for LED output, ALL serial input is disabled. On M5StickC with USB CDC serial, these pin checks need to be bypassed or adapted.

### Protocol State Machine

Single `AdaState` enum multiplexes all protocols by first byte:

```cpp
enum class AdaState {
  Header_A,           // Adalight 'A'
  Header_d,           // Adalight 'd'
  Header_a,           // Adalight 'a'
  Header_CountHi,
  Header_CountLo,
  Header_CountCheck,
  Data_Red,
  Data_Green,
  Data_Blue,
  TPM2_Header_Type,   // TPM2 0xC9
  TPM2_Header_CountHi,
  TPM2_Header_CountLo,
};
```

### Dispatch Table (First Byte)

| First Byte | Protocol | Action |
|---|---|---|
| `'A'` (0x41) | Adalight | Header state machine → realtime RGB pixel data |
| `0xC9` | TPM2 | Header state machine → realtime RGB pixel data |
| `'I'` (0x49) | Improv Serial | `handleImprovPacket()` — WiFi provisioning |
| `'v'` (0x76) | Version query | Prints `"WLED <VERSION>"` |
| `0xB0`–`0xB7` | Baud rate change | Switches baud (115200 to 1500000) |
| `'l'` (0x6C) | LED data (JSON) | Returns pixel colors as JSON array |
| `'L'` (0x4C) | LED data (TPM2) | Returns pixel colors as TPM2 binary packet |
| `'o'`/`'O'` | Continuous stream | Disable/enable continuous LED data streaming |
| **`'{'` (0x7B)** | **JSON API** | **Deserializes JSON, calls `deserializeState()`** |

### JSON API Over Serial — Critical Code Path

```cpp
// wled_serial.cpp lines 106-129
else if (next == '{') {  //JSON API
  bool verboseResponse = false;
  if (!requestJSONBufferLock(JSON_LOCK_SERIAL)) {
    Serial.printf_P(PSTR("{\"error\":%d}\n"), ERR_NOBUF);
    return;
  }
  Serial.setTimeout(100);
  DeserializationError error = deserializeJson(*pDoc, Serial);
  if (!error) {
    verboseResponse = deserializeState(pDoc->as<JsonObject>());
    if (verboseResponse && serialCanTX) {
      pDoc->clear();
      JsonObject stateDoc = pDoc->createNestedObject("state");
      serializeState(stateDoc);
      JsonObject info  = pDoc->createNestedObject("info");
      serializeInfo(info);
      serializeJson(*pDoc, Serial);
      Serial.println();
    }
  }
  releaseJSONBufferLock();
}
```

**Observations**:
1. JSON read directly from `Serial` stream with 100ms timeout
2. Response only sent if `{"v":true}` was in the request (sets `verboseResponse`)
3. Response format: `{"state":{...},"info":{...}}\n`
4. Uses shared JSON buffer with locking (`requestJSONBufferLock`)
5. Buffer lock ID is `JSON_LOCK_SERIAL`

### Baud Rate Switch Commands

| Byte | Baud Rate |
|---|---|
| 0xB0 | 115200 |
| 0xB1 | 230400 |
| 0xB2 | 460800 |
| 0xB3 | 500000 |
| 0xB4 | 576000 |
| 0xB5 | 921600 |
| 0xB6 | 1000000 |
| 0xB7 | 1500000 |

## JSON API — State Object

### `deserializeState()` Top-Level Keys

| Key | Type | Description |
|---|---|---|
| `on` | bool/`"t"` | On/off, supports toggle |
| `bri` | 0-255 | Brightness |
| `transition` | 0-65535 | Crossfade (100ms units) |
| `tt` | 0-65535 | Temporary transition (this call only) |
| `ps` | preset ID | Select preset (cycling: `"1~5~"`, random: `"1~5r"`) |
| `psave` | 1-250 | Save current state to preset |
| `pdel` | 1-250 | Delete preset |
| `nl` | object | Nightlight: `{on, dur, mode, tbri}` |
| `udpn` | object | UDP sync: `{send, recv, sgrp, rgrp, nn}` |
| `time` | uint32 | Set system time (unix) |
| `rb` | bool | Reboot |
| `lor` | 0-2 | Live data override mode |
| `live` | bool | Enter/exit realtime mode |
| `mainseg` | int | Main segment ID |
| `seg` | object/array | Segment configuration |
| `playlist` | object | Playlist: `{ps[], dur[], transition, repeat, end}` |
| `v` | bool | Request verbose response |
| `wifi` | object | WiFi control: `{ap}` |

### Segment Object Keys

| Key | Description |
|---|---|
| `id`, `start`, `stop`, `len` | Segment bounds |
| `grp`, `spc`, `of` | Grouping, spacing, offset |
| `col` | Colors: `[[R,G,B,W],[R,G,B,W],[R,G,B,W]]` or hex |
| `fx` | Effect mode ID |
| `sx`, `ix` | Speed, intensity |
| `pal` | Palette ID |
| `c1`, `c2`, `c3` | Custom sliders |
| `on`, `bri`, `sel`, `rev`, `mi` | On/off, brightness, selected, reverse, mirror |
| `frz` | Freeze effect |
| `i` | Individual LED control array |
| `n` | Segment name |

### HTTP JSON Endpoints (for serial parity)

| Endpoint | Method | Description |
|---|---|---|
| `/json` | GET | Returns `{state, info, effects, palettes}` |
| `/json/state` | GET/POST | State object only |
| `/json/info` | GET | Info object only |
| `/json/eff` | GET | Effects array |
| `/json/pal` | GET | Palettes array |
| `/json/fxdata` | GET | Effect metadata array |
| `/json/live` | GET | Live LED data |

### Key Serialization Functions

| Function | Location | Purpose |
|---|---|---|
| `serializeState()` | json.cpp:647 | Serialize current state |
| `serializeInfo()` | json.cpp:706 | Serialize device info |
| `serializePalettes()` | json.cpp:982 | Serialize palette data |
| `serializeNodes()` | json.cpp:1091 | Serialize discovered nodes |

## UDP Realtime Protocols

### From `udp.cpp` — `handleNotifications()`

| Protocol | Port | Realtime Mode |
|---|---|---|
| WLED Notifier (sync) | 21324 | N/A (state sync) |
| Hyperion (raw RGB) | 19446 | `REALTIME_MODE_HYPERION` |
| TPM2.NET | 65506 | `REALTIME_MODE_TPM2NET` |
| UDP Realtime (WARLS/DRGB/DRGBW/DNRGB) | 21324 | `REALTIME_MODE_UDP` |
| UDP JSON API | 21324 | N/A |

### From `e131.cpp`

| Protocol | Port | Mode |
|---|---|---|
| E1.31 (sACN) | 5568 | `REALTIME_MODE_E131` |
| Art-Net | 6454 | `REALTIME_MODE_ARTNET` |
| DDP | 4048 | `REALTIME_MODE_DDP` |

### Realtime Lock Mechanism

`realtimeLock(uint32_t timeoutMs, byte md)` — called by ALL realtime sources. Clears strip or freezes main segment, sets `realtimeMode`, sets auto-exit timeout.

## Build Flags — Module Disable/Enable

| Build Flag | Default | Saves | Description |
|---|---|---|---|
| `-D WLED_DISABLE_OTA` | enabled | ~14KB | Disable OTA updates |
| `-D WLED_DISABLE_ALEXA` | enabled | ~11KB | Disable Alexa/Espalexa |
| `-D WLED_DISABLE_HUESYNC` | enabled | ~4KB | Disable Philips Hue sync |
| `-D WLED_DISABLE_INFRARED` | enabled | ~12KB | Disable IR remote |
| `-D WLED_DISABLE_MQTT` | enabled | ~12KB | Disable MQTT |
| **`-D WLED_DISABLE_ADALIGHT`** | enabled | ~5KB | **Disables ALL serial RX** |
| `-D WLED_DISABLE_LOXONE` | enabled | ~1.2KB | Disable Loxone |
| `-D WLED_DISABLE_WEBSOCKETS` | enabled | varies | Disable WebSocket |
| `-D WLED_DISABLE_ESPNOW` | enabled | varies | Disable ESP-NOW |
| `-D WLED_DISABLE_2D` | enabled | varies | Disable 2D matrix |
| `-D WLED_DISABLE_BROWNOUT_DET` | disabled | 0 | Disable brownout detector |

**WARNING**: `WLED_DISABLE_ADALIGHT` disables the entire `handleSerial()` function — including JSON over serial. Do NOT set this flag for the serial controller project.

## Custom PlatformIO Environment Pattern

Use `platformio_override.ini`:

```ini
[env:m5stickc_serial]
extends = esp32_idf_V5
board = m5stick-c
platform = ${esp32_idf_V5.platform}
build_flags = ${common.build_flags} ${esp32_idf_V5.build_flags}
  -D WLED_RELEASE_NAME=\"M5StickC_Serial\"
  -D WLED_DISABLE_ALEXA
  -D WLED_DISABLE_HUESYNC
  -D WLED_DISABLE_INFRARED
  -D WLED_DISABLE_MQTT
  -D WLED_DISABLE_ESPNOW
  -D WLED_DISABLE_OTA
  -D WLED_DISABLE_2D
  -D DATA_PINS=26
  -DARDUINO_USB_CDC_ON_BOOT=0
```

## Usermod System

Usermods added via `custom_usermods` in `platformio_override.ini`, referencing folders under `usermods/`. Self-register at compile time — no manual `#include` editing needed.

## Critical Globals

| Global | Type | Purpose |
|---|---|---|
| `bri` | uint8_t | Master brightness |
| `strip` | WS2812FX | LED strip object — effects, segments, pixel data |
| `pDoc` | JsonDocument* | Shared JSON buffer (heap, ~24KB on ESP32) |
| `realtimeMode` | byte | Current realtime source (0=inactive) |
| `serialCanRX` / `serialCanTX` | bool | Whether serial pins are available |
| `stateChanged` | bool | Triggers state update notifications |

## Prior Art: `miwied/wled-json-api-over-serial`

**Repo**: https://github.com/miwied/wled-json-api-over-serial

A **client-side example** (not a WLED fork):
- ESP32 sends JSON commands over hardware UART to a separate WLED-flashed ESP8266
- Uses ArduinoJson to construct `{"on":true,"bri":255,"seg":[{"col":[[R,G,B]]}]}` payloads
- Tested baud rates: 115200 through 1500000
- **Two-MCU architecture**: one runs WLED, another sends commands

Our project runs WLED on the same MCU and exposes the API over USB serial to a host PC — eliminating the second MCU.

## Documentation URLs

| Resource | URL |
|---|---|
| JSON API | https://kno.wled.ge/interfaces/json-api/ |
| Serial Interface | https://kno.wled.ge/interfaces/serial/ |
| UDP Realtime | https://kno.wled.ge/interfaces/udp-realtime/ |
| E1.31/Art-Net | https://kno.wled.ge/interfaces/e1.31-dmx/ |
| DDP | https://kno.wled.ge/interfaces/ddp/ |
| Compiling | https://kno.wled.ge/advanced/compiling-wled/ |
| Custom Features | https://kno.wled.ge/advanced/custom-features/ |
