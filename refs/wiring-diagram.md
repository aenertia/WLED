# Wiring Diagram — WLED USB ARGB Controller

## Pico Bridge Variant (Recommended)

All components inside the PC case. Zero external cables.

### Wire List

| # | From | To | Wire | Notes |
|---|---|---|---|---|
| 1 | Mobo USB2 header VCC | Pico VBUS | Dupont F-F | Powers Pico from mobo |
| 2 | Mobo USB2 header D- | Pico D- | Dupont F-F | USB data |
| 3 | Mobo USB2 header D+ | Pico D+ | Dupont F-F | USB data |
| 4 | Mobo USB2 header GND | Pico GND | Dupont F-F | Common ground |
| 5 | Pico GP0 (TX) | M5StickC G33 (RX) | Dupont F-F | UART PPP (5Mbps) |
| 6 | Pico GP1 (RX) | M5StickC G32 (TX) | Dupont F-F | UART PPP (5Mbps) |
| 7 | Pico GND | M5StickC GND | Dupont/shared | Common ground |
| 8 | M5StickC G26 | 74AHCT125 1A | Dupont | 3.3V LED data |
| 9 | 74AHCT125 1Y | First ARGB DIN | Dupont | 5V LED data out |
| 10 | Mobo ARGB header Data | M5StickC G36 | Dupont | Motherboard ARGB passthrough |
| 11 | Mobo ARGB header 5V | LED strip 5V + 74AHCT125 VCC | Dupont | LED power from mobo |
| 12 | Mobo ARGB header GND | Common GND bus | Dupont | Shared ground |

**Total: 12 connections, all Dupont female-female, no soldering.**

### Signal Flow

```
Host OS                Pi Pico               M5StickC (ESP32)            LEDs
                       (USB-C clone)         (wled.local)
                       
[CDC-NCM]  <--USB-->  [TinyUSB]             
                       [lwIP+PPP]            
                        GP0 TX  ---------->  G33 RX (UART1)
                        GP1 RX  <----------  G32 TX (UART1)
                                             [esp_netif PPP 5Mbps]
                                             [WLED AsyncWebServer]
                                             [DDP :4048 / E1.31]
                                             
                                             G26 (RMT TX) --> [74AHCT125] --> Fan1 --> Fan2 --> Strip
                                             
Motherboard                                  
  ARGB Header Data  ---------------------->  G36 (RMT RX, input-only)
  ARGB Header 5V   -------------------------------------------------> LED 5V rail
  ARGB Header GND  -------------------------------------------------> Common GND
```

### Boot Sequence

1. PC powers on -> mobo drives ARGB header -> data flows to G36 -> ESP32 RMT RX captures -> mirrors to G26 -> LEDs show mobo POST colors
2. OS loads -> pppd auto-starts (udev rule) -> Pico USB enumerates as CDC-NCM Ethernet
3. Pico PPP connects to ESP32 via UART1 (5Mbps)
4. ESP32 gets IP (169.254.7.1) -> mDNS announces wled.local -> exitRealtime()
5. WLED effects take over LEDs -> dashboard at http://wled.local
6. OpenRGB sends DDP to wled.local:4048 -> realtime pixel control

## Motherboard USB 2.0 Internal Header Pinout

Standard 9-pin (2x5, 1 blocked):

```
Pin 1: VCC (+5V)     Pin 2: VCC (+5V)
Pin 3: D-            Pin 4: D-
Pin 5: D+            Pin 6: D+
Pin 7: GND           Pin 8: GND
Pin 9: (key/blocked) Pin 10: (NC)
```

Each column is one USB port. Use ONE column (pins 1,3,5,7 or 2,4,6,8).

## Motherboard ARGB Header Pinout (3-pin 5V)

```
Pin 1: +5V (LED power)
Pin 2: Data (WS2812B signal)
Pin 3: (blocked/key)
Pin 4: GND
```

## GPIO Map — Pico Variant

| GPIO | Function | Direction | Connected To |
|---|---|---|---|
| G32 (Grove yellow) | UART1 TX | OUT | Pico GP1 (RX) |
| G33 (Grove white) | UART1 RX | IN | Pico GP0 (TX) |
| G26 (HAT) | LED data (RMT TX) | OUT | 74AHCT125 -> ARGB chain |
| G36 (HAT) | ARGB passthrough (RMT RX) | IN | Mobo ARGB header data |
| GPIO1/3 (UART0) | Free (FTDI idle) | — | M5StickC micro-USB (debug) |
| SPI (G15/13/5/23/18) | TFT display | OUT | Built-in ST7735S |

## GPIO Map — FTDI Variant (no Pico)

| GPIO | Function | Direction | Connected To |
|---|---|---|---|
| GPIO1/3 (UART0) | PPP via FTDI | BIDIR | M5StickC micro-USB -> host |
| G26 (HAT) | LED data (RMT TX) | OUT | 74AHCT125 -> ARGB chain |
| G32 (Grove yellow) | ARGB passthrough (RMT RX) | IN | Mobo ARGB header data |
| G33 (Grove white) | Free | — | — |
| G36 (HAT) | Free | — | — |
| SPI (G15/13/5/23/18) | TFT display | OUT | Built-in ST7735S |

## Power Architecture

```
PSU SATA/Molex -> Motherboard
  |
  +-- Mobo USB2 header 5V --> Pi Pico VBUS (powers Pico)
  +-- Mobo ARGB header 5V --> LED strip 5V + 74AHCT125 VCC
  
M5StickC: 
  Option A: Self-powered via internal 95mAh battery (limited runtime)
  Option B: Powered from Pico 3.3V out -> M5StickC 3.3V HAT pin
  Option C: Separate USB cable to mobo USB header (uses 2nd USB port)
  Option D: Tap 5V from ARGB header -> M5StickC 5V HAT pin
```

**Recommended: Option D** — tap 5V from mobo ARGB header to M5StickC HAT 5V pin. Single power source for everything (PSU -> mobo -> ARGB header -> both LEDs and M5StickC). No separate USB cable needed.

## Signal Integrity Notes

- **UART (Pico<->ESP32)**: 3.3V both sides, no level shifting. Keep wires <10cm at 5Mbps.
- **LED data (G26->strip)**: 74AHCT125 level shifter mandatory (3.3V -> 5V).
- **ARGB passthrough (mobo->G36)**: Mobo ARGB outputs 5V data. ESP32 GPIO is rated 3.3V but 5V tolerant in practice. Recommended: voltage divider (10K + 20K) for reliability, or direct connection (works for most users).

## Component BOM

| Component | Qty | Est. Cost | Source |
|---|---|---|---|
| M5StickC (ESP32-PICO-D4) | 1 | $12 | M5Stack / AliExpress |
| Pi Pico clone (16MB, USB-C) | 1 | $3 | WeAct / AliExpress |
| 74AHCT125 level shifter | 1 | $0.50 | LCSC / DigiKey |
| Dupont F-F jumper wires | 12 | $1 | Any |
| **Total** | | **~$16.50** | |
