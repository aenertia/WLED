# PC ARGB Hardware Ecosystem Reference

## ARGB Connector Standards

### 3-Pin 5V ARGB Header (Addressable)

Standard PC motherboard ARGB header — physically 4-pin with 1 keyed/blocked position:

| Pin | Function | Notes |
|---|---|---|
| 1 | **+5V** | Power supply for LEDs |
| 2 | **Data** | Serial data (WS2812B-compatible) |
| 3 | *(blocked)* | Key pin — physically absent |
| 4 | **GND** | Ground |

Connector type: JST SM 3-pin or proprietary. Pin pitch 2.54mm (0.1").

### 4-Pin 12V RGB Header (Non-Addressable — excluded)

| Pin | Function |
|---|---|
| 1 | +12V |
| 2 | Green (PWM) |
| 3 | Red (PWM) |
| 4 | Blue (PWM) |

Non-addressable — all LEDs show same color via MOSFET PWM. **Not targeted by this project.**

## LED Chips Used in PC ARGB

PC ARGB devices overwhelmingly use **WS2812B or protocol-compatible chips**:

| Chip | Voltage | Notes |
|---|---|---|
| **WS2812B** | 5V | De facto standard for PC ARGB |
| SK6812 | 5V/12V | RGBW variant, WS2812B-compatible protocol |
| WS2811 | 5V/12V | Pixel strings; 12V versions use 3-LED segments |
| WS2813 | 5V | Backup data line for fault tolerance |
| WS2815 | 12V | Backup data line, 12V variant |

### WS2812B Data Protocol

Single-wire NRZ (Non-Return-to-Zero):

| Parameter | Value |
|---|---|
| Bit period | ~1.25 us |
| Logic 0 | ~0.4 us high, ~0.85 us low |
| Logic 1 | ~0.8 us high, ~0.45 us low |
| Reset | >280 us low |
| Data order | **GRB** (Green-Red-Blue), 8 bits/channel, MSB first |
| Data rate | 800 Kbps |

## Typical LED Counts Per PC Device

### Fans

| Device | LEDs |
|---|---|
| Budget fans | 4-8 |
| Corsair HD120 | 12 |
| NZXT Aer 2 (120/140mm) | 8 |
| Corsair LL120 | 16 |
| NZXT F120 RGB | 18 |
| NZXT F120 RGB Duo | 20 |
| NZXT F360 RGB Core | 24 |
| **Corsair QL120** | **34** (dual-ring) |

### Strips & Accessories

| Device | LEDs |
|---|---|
| NZXT Hue 2 strip (short) | 6-8 |
| NZXT Hue 2 strip (long) | 10-15 |
| NZXT Cable Comb | 14 |
| NZXT Underglow (300mm) | 15 |
| AIO cooler rings | 8-24 |
| 1m strip @ 30/m | 30 |
| 1m strip @ 60/m | 60 |

## Commercial USB ARGB Controllers

### Corsair Lighting Node Pro / Commander Pro

| Feature | Lighting Node Pro | Commander Pro |
|---|---|---|
| USB Interface | **USB HID** | **USB HID** |
| VID:PID | `1B1C:0C0B` | `1B1C:0C10` |
| RGB Channels | 2 | 2 |
| Fan Channels | 0 | 6 (PWM) |
| Temp Sensors | 0 | 2 |
| Max LEDs/channel | ~204 | Same |
| Power input | SATA | SATA |
| Protocol | Proprietary HID (reverse-engineered) | Same family |

Protocol details:
- Colors sent as separate R, G, B channel packets (not interleaved)
- 50 LEDs per packet, batched with offset
- Requires keepalive every 5 seconds or device reverts to rainbow mode
- Port state must be set to `SOFTWARE` (0x02) for direct control

### NZXT HUE 2 / Smart Device

| Feature | HUE 2 | Smart Device V2 |
|---|---|---|
| USB Interface | USB HID | USB HID |
| RGB Channels | Up to 4 | Up to 6 |
| Fan Channels | 0 | 3 |
| Max LEDs/channel | 40 | Same |
| Auto-detection | Yes (per-channel) | Yes |

Protocol: Interleaved GRB (not RGB). 64-byte HID packets.

### Razer Chroma ARGB Controller

- USB HID, 6 ARGB channels
- Proprietary Razer protocol
- Supported by OpenRGB (reverse-engineered)

### Open-Source: CorsairLightingProtocol

**GitHub**: https://github.com/Legion2/CorsairLightingProtocol (571+ stars)

- Emulates Corsair Lighting Node PRO, Commander PRO, Lighting Node CORE
- USB HID on ATmega32U4 (Leonardo/Pro Micro) or TinyUSB (RP2040)
- Compatible with iCUE, OpenRGB, SignalRGB, RGBSync
- Uses FastLED for LED output
- 2 channels, up to 204 LEDs per channel
- **NOT compatible with standard ESP32** (requires native USB)

## USB Interface: HID vs CDC

### All Major Commercial Controllers Use USB HID

| Controller | Interface | Packet Size |
|---|---|---|
| Corsair LNP/Commander | USB HID | 65B write, 17B read |
| NZXT HUE 2 / Smart Device | USB HID | 64B bidirectional |
| Razer Chroma ARGB | USB HID | 64B |

Why HID:
- No driver installation (natively supported)
- Structured fixed-size packets
- Bidirectional status reporting
- VID/PID auto-discovery

CDC Serial is used by: WLED, Adalight/Prismatik, DIY projects prioritizing simplicity.

## Power Architecture

### Commercial Controller Power Path

```
                    +---------------------+
  SATA Power ------►|  ARGB Controller    |
  (from PSU)        |  (Corsair/NZXT)     |
                    |                     |
  USB Data --------►|  MCU + Regulator    |
  (from mobo)       |                     |
                    +--+---+---+---+-----+
                       |   |   |   |     ARGB Cables (5V+Data+GND)
                    +--v-++v--++v--+
                    |Fan1||Fan2||Fan3|
                    +----++---++---+
```

- PSU SATA → controller board → 5V rail to ARGB headers
- MCU powered from USB 5V (low current) or SATA 5V
- LED power from SATA, **not** from USB
- Motherboard ARGB headers: powered from board 5V rail, typically **3A limit per header**

### Current Draw Per LED (WS2812B)

| Condition | Current |
|---|---|
| Single color full brightness | ~20 mA |
| Full white (R+G+B all 255) | ~60 mA |
| Typical mixed color usage | ~20-30 mA avg |
| All off | ~1 mA quiescent |

### Power Budget Examples

| Config | LEDs | Max Current | Typical |
|---|---|---|---|
| 8-LED fan | 8 | 480 mA | ~200 mA |
| 18-LED fan | 18 | 1.08 A | ~400 mA |
| 34-LED Corsair QL | 34 | 2.04 A | ~800 mA |
| 60-LED strip (1m) | 60 | 3.6 A | ~1.5 A |
| 144-LED strip (1m) | 144 | 8.64 A | ~3.5 A |

**USB alone cannot power significant LED loads** (500 mA USB 2.0, 900 mA USB 3.0). External 5V PSU is required.

## Daisy-Chaining & Addressing

WS2812B uses shift-register cascade — **no explicit addressing**:

```
Controller --Data--> LED0 --Data Out--> LED1 --Data Out--> LED2 --> ...
```

1. Controller sends continuous stream of 24-bit GRB values
2. First LED latches first 24 bits, passes rest downstream
3. Reset period (>280 us low) signals end of frame — all update simultaneously

Key implications:
- Position in chain = address (no addressing protocol)
- LED count must be configured in software
- Chain length limited by signal degradation (~5m practical max)
- Refresh rate: ~30 us per LED + 280 us reset = ~800 LEDs at 30fps
- Commercial controllers use multiple independent channels (2-6) for parallel update

## Recommended Level Shifter

**SN74AHCT125** — Aircoookie's (WLED creator) recommended level shifter.

- Single chip, powered from 5V
- HCT family accepts 3.3V input as logic high (threshold ~1.4V)
- Also: 74HCT245 (8-channel bidirectional)

## Design Implications Summary

| Aspect | Recommendation |
|---|---|
| LED Protocol | WS2812B (800kHz NRZ, GRB) — covers 95%+ of PC ARGB |
| Connector | Standard 3-pin ARGB (5V, Data, GND) |
| Channels | 1-2 (M5StickC has limited GPIOs) |
| USB Interface | CDC serial (Adalight) for simplicity |
| Power | External 5V, NOT from USB |
| Level Shifting | SN74AHCT125 from 3.3V to 5V |
| Max LEDs | Plan for ~200 LEDs per channel |
| Software | OpenRGB (Adalight config), WLED apps, custom JSON |
