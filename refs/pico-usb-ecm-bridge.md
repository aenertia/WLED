# Pi Pico USB-ECM Bridge — ADR Extension

## Problem

PPP-over-serial requires `pppd` on the host — an extra dependency, manual setup, and no Windows support without WSL. Can we eliminate this by adding an RP2040 (Pi Pico) as a USB Ethernet bridge?

## Concept

The Pi Pico presents as a standard USB Ethernet adapter (CDC-NCM) to the host PC. IP packets are bridged over SPI to the ESP32 running WLED. The host sees a regular network interface — zero drivers, zero pppd, zero config.

```
Host PC                     Pi Pico (RP2040)              M5StickC (ESP32)
┌──────────┐  USB 12Mbps  ┌──────────────┐  SPI ~10MHz  ┌──────────────┐
│ Browser  │◄════════════►│ TinyUSB      │◄════════════►│ esp_netif    │
│ OpenRGB  │  CDC-NCM     │ CDC-NCM      │  eppp_link   │ WLED         │
│          │  (Ethernet)  │ Frame bridge │  or raw ETH  │ AsyncWebSvr  │
│ ppp0: NO │              │              │              │ DDP/E1.31    │
│ eth/usb: YES            │ lwIP routing │              │ Effects      │
└──────────┘              └──────────────┘              └──────────────┘
                           16MB flash                    G26 → LEDs
```

## USB Protocol: CDC-NCM (Recommended)

| Protocol | Linux | macOS | Windows 10+ | Windows 7 | Throughput |
|---|---|---|---|---|---|
| **CDC-NCM** | Native | Native | **Native** | Needs driver | Best (packet aggregation) |
| CDC-ECM | Native | Native | Needs Zadig | No | Good |
| RNDIS | Driver | No | Native | Native | OK (deprecated by MS) |

**CDC-NCM wins** — native zero-config on Linux, macOS, AND Windows 10+. NCM adds packet aggregation (multiple Ethernet frames per USB transfer) for better throughput than ECM.

TinyUSB supports all three on RP2040 via `CFG_TUD_NET`. The `net_lwip_webserver` example demonstrates this.

## RP2040 Specifications

| Parameter | Value |
|---|---|
| USB | Native USB 1.1 Full Speed (12 Mbps) |
| Effective USB throughput | ~8-10 Mbps after CDC-NCM overhead |
| SPI master | Up to 62.5 MHz (PL022) |
| UART | Up to 7.8 Mbps |
| PIO | 2× programmable I/O blocks (custom protocols) |
| RAM | 264 KB SRAM |
| Flash (16MB clone) | 16 MB (WeAct Studio, YD-RP2040, etc.) |
| SDK | Pico SDK + lwIP included |
| Cost | ~$2-4 USD (clone), ~$4 official |
| Size | 51×21mm (similar to M5StickC 48×24mm) |

### 16MB Flash Clones

| Board | Flash | USB | Price | Notes |
|---|---|---|---|---|
| WeAct Studio RP2040 | 16MB | USB-C | ~$3 | Popular, well-tested |
| YD-RP2040 | 16MB | USB-C | ~$3 | VCC-GND brand |
| Waveshare RP2040-Zero | 16MB | USB-C | ~$4 | Tiny (23×18mm) |
| Official Pi Pico | 2MB | Micro-USB | $4 | Only 2MB flash |

16MB is massive overkill for a bridge firmware (~50-100KB), but headroom for future features.

## Architecture Options

### Option A: Raw Ethernet Frame Bridge (RECOMMENDED)

Simplest approach — Pico has NO IP stack, just forwards raw Ethernet frames between USB and SPI.

```
Host USB → CDC-NCM → TinyUSB → raw Ethernet frame → SPI TX →
  ESP32 SPI RX → esp_netif (Ethernet-like driver) → lwIP → WLED
```

**Pico firmware** (~200 lines):
- TinyUSB CDC-NCM device: receives Ethernet frames from host
- SPI master: forwards frames to ESP32 with simple length-prefix framing
- Bidirectional: ESP32 → SPI → Pico → USB → host for responses
- No lwIP on Pico — just frame relay

**ESP32 firmware**:
- SPI slave receives raw Ethernet frames
- Custom `esp_netif` driver wraps SPI as an Ethernet-like interface
- WLED's `WLEDNetworkClass` sees it like Ethernet (same as `isEthernet()` pattern)

**Pros**: Simplest Pico firmware, all IP handling on ESP32 (proven), lowest latency
**Cons**: Custom SPI framing protocol (simple but non-standard)

### Option B: eppp_link PPP-over-SPI

Use Espressif's `eppp_link` component with SPI transport. Pico runs a PPP stack over SPI.

```
Host USB → CDC-NCM → TinyUSB → lwIP (Pico) → PPP → SPI →
  ESP32 SPI → eppp_link → esp_netif PPP → lwIP → WLED
```

**Pico firmware** (~500 lines):
- TinyUSB CDC-NCM + lwIP for USB-side IP
- lwIP PPP client or raw IP forwarding over SPI
- SPI master matching `eppp_link` protocol

**ESP32 firmware**:
- `eppp_link` with SPI transport (existing Espressif component)
- Same `esp_netif` integration as our UART PPP (minimal changes)

**Pros**: Uses proven `eppp_link` component on ESP32, well-tested SPI framing
**Cons**: Two lwIP stacks (wasteful), more complex Pico firmware, PPP overhead

**eppp_link SPI benchmarks** (from Espressif):
- SPI@20MHz: ~16 Mbps TCP, ~16 Mbps UDP
- SPI@10MHz: ~8 Mbps TCP, ~8 Mbps UDP
- Exceeds USB Full Speed (12 Mbps) — SPI is not the bottleneck

### Option C: ESP-Hosted Pattern

Espressif's `esp_hosted` uses ESP32 as co-processor. We'd adapt this pattern with RP2040 as the USB host MCU.

**Too complex for our use case.** ESP-Hosted is designed for Linux hosts with full driver stacks. Our Pico is a simple bridge, not a Linux host.

## Comparison: Dual MCU vs Single ESP32-S3

| Factor | ESP32 + Pi Pico | ESP32-S3 (single board) |
|---|---|---|
| USB | Native RP2040 (12 Mbps) | Native S3 (12 Mbps, or 480 Mbps with ext PHY) |
| CDC-NCM | TinyUSB on Pico | TinyUSB on S3 (supported) |
| Complexity | 2 MCUs, 2 firmwares, SPI bridge | 1 MCU, 1 firmware |
| Display | M5StickC 80×160 TFT | M5AtomS3: 128×128 OLED (smaller) |
| Form factor | M5StickC + Pico (two boards) | Single board |
| LED output | ESP32 RMT (proven) | ESP32-S3 RMT (proven, + DMA) |
| Cost | ~$12 + ~$3 = ~$15 | ~$8-15 |
| Our existing work | PPP code reusable | Need to port to S3, but simpler |
| WLED support | ESP32 (well-tested) | ESP32-S3 (well-tested, in upstream) |

**Single ESP32-S3 is objectively simpler** — one board, one firmware, native USB-ECM, no SPI bridge. But the M5StickC has the nice 80×160 TFT and the user already has it.

### Candidate ESP32-S3 Boards

| Board | USB | Display | Flash | Size | Price |
|---|---|---|---|---|---|
| M5AtomS3 | USB-C native | 128×128 OLED | 8MB | 24×24mm | ~$8 |
| M5AtomS3 Lite | USB-C native | None | 8MB | 24×24mm | ~$6 |
| M5StampS3 | USB-C native | None | 8MB | 14×14mm | ~$5 |
| M5NanoC6 | USB-C native | None | 4MB | 12×12mm | ~$4 (ESP32-C6) |

## Physical Integration (Pico + M5StickC)

### SPI Wiring (4 data + 2 power)

| RP2040 (Pico) | ESP32 (M5StickC) | Function |
|---|---|---|
| GP18 (SPI0 SCK) | G32 (Grove yellow) | SPI Clock |
| GP19 (SPI0 TX/MOSI) | G33 (Grove white) | Pico → ESP32 data |
| GP16 (SPI0 RX/MISO) | G26 (HAT) | ESP32 → Pico data |
| GP17 (SPI0 CSn) | G0 (HAT) | Chip select |
| GP20 | G36 (HAT, input) | Handshake/data-ready |
| GND | GND | Common ground |

**Problem**: This uses G26 for SPI MISO, but G26 is our LED data output. Need to either:
- Use a different GPIO for LEDs (G32 → LED, G26 → SPI) — requires rewiring
- Use UART instead of SPI (only 2 wires: TX+RX, leaves G26 free for LEDs)
- Add the Pico to the USB chain instead of connecting to M5StickC GPIOs

### UART Alternative (simpler, leaves G26 free)

| RP2040 (Pico) | ESP32 (M5StickC) | Function |
|---|---|---|
| GP0 (UART0 TX) | G33 (Grove white, RX) | Pico → ESP32 |
| GP1 (UART0 RX) | G32 (Grove yellow, TX) | ESP32 → Pico |
| GND | GND | Common ground |

UART at 3-4 Mbps (RP2040 supports up to 7.8 Mbps). `eppp_link` measured 2 Mbps TCP over UART@3Mbaud — exceeds USB 12 Mbps bottleneck after CDC-NCM overhead.

**G26 stays free for LED output.**

### USB Chain Option (most elegant)

```
PC USB → Pi Pico (USB-C) → CDC-NCM bridge + UART TX/RX →
  M5StickC (USB Micro) → FTDI UART → ESP32 → PPP (existing code!)
```

Wait — this is just using the Pico as a USB Ethernet-to-serial converter that talks PPP to the existing M5StickC firmware over the M5StickC's own USB port. No SPI needed, no GPIO wiring, no firmware changes on ESP32!

The Pico intercepts the USB connection:
1. Host sees Pico as USB Ethernet (CDC-NCM)
2. Pico's lwIP routes IP packets
3. Pico converts to PPP and sends over its UART to M5StickC's USB
4. M5StickC's FTDI converts UART to USB... wait, that doesn't work — both are USB devices

**Actually**: Connect Pico UART directly to M5StickC Grove port (UART). Pico bridges USB-ECM ↔ UART-PPP. ESP32 runs our existing PPP code unchanged. This is **the simplest option**:
- Zero ESP32 firmware changes (existing PPP code handles UART)
- Pico acts as USB-ECM ↔ PPP-over-UART bridge
- Host sees standard USB Ethernet adapter
- One USB cable (to Pico), two wires (UART) to M5StickC

## Recommendation

**For maximum simplicity with M5StickC**: Use Pi Pico as CDC-NCM ↔ PPP-over-UART bridge. Pico has USB-ECM facing the host and UART facing the ESP32. Our existing PPP firmware on ESP32 works unchanged — the Pico is just a smarter FTDI that presents as Ethernet instead of serial.

**For long-term / v2**: Switch to ESP32-S3 (M5AtomS3). Native USB-OTG with CDC-ECM/NCM. One board, one firmware, zero bridge complexity.

## Estimated Effort

| Approach | Pico Firmware | ESP32 Changes | Total Effort |
|---|---|---|---|
| Pico CDC-NCM → UART PPP bridge | ~300 lines (TinyUSB + lwIP + PPP) | **Zero** | 2-3 days |
| Pico raw ETH → SPI bridge | ~200 lines | ~100 lines (SPI netif driver) | 3-4 days |
| Pico eppp_link SPI | ~500 lines | ~50 lines (eppp_link config) | 4-5 days |
| ESP32-S3 single board | N/A | ~50 lines (USB-ECM netif) | 2 days + port |

## Fastest Path: UART via Grove at 3-4 Mbps

### GPIO Constraint Analysis

M5StickC available GPIOs:
- G26 (HAT): LED data output — MUST stay free
- G32 (Grove yellow): Available
- G33 (Grove white): Available
- G0 (HAT): Strapping pin — risky
- G36 (HAT): Input-only — can't be SPI MISO

SPI needs 4 pins (SCK+MOSI+MISO+CS) — we only have 3 usable (G26+G32+G33),
and G26 is reserved for LEDs. SPI is physically impossible without sacrificing LED output.

**UART needs 2 pins** — G32 (TX) + G33 (RX) on Grove. G26 stays free. Clean.

### Baud Rate Capabilities

| MCU | Max UART | Clock Source |
|---|---|---|
| ESP32 | ~5 Mbps | APB 80 MHz |
| RP2040 | ~7.8 Mbps | System clock / 8 |
| **Common max** | **~4 Mbps** | Both comfortable |

Target: **3 Mbps** (proven in eppp_link benchmarks) or 4 Mbps (within both chips' range).

### Throughput Budget

```
USB Full Speed:     12 Mbps raw → ~8 Mbps effective (CDC-NCM overhead)
UART at 3 Mbps:    3 Mbps raw → ~2 Mbps TCP throughput (eppp_link measured)
Bottleneck:         USB (8 Mbps) > UART (2 Mbps) — UART is the limiter
```

But for our actual traffic:
| Traffic | Bandwidth | % of 2 Mbps |
|---|---|---|
| Dashboard initial load (55KB) | Burst: 0.22s | — |
| WebSocket state updates (3KB/s) | 24 kbps | 1.2% |
| DDP 300 LEDs @ 30fps (27KB/s) | 216 kbps | 10.8% |
| WS live preview 300 LEDs (22KB/s) | 176 kbps | 8.8% |
| **All simultaneous** | **~440 kbps** | **22%** |

78% headroom. **UART at 3 Mbps is not the bottleneck for any realistic workload.**

### Wiring (final)

```
Pi Pico (USB-C)              M5StickC (Grove HY2.0-4P)
┌──────────────┐            ┌──────────────┐
│ GP0 (UART TX)├────────────┤ G33 (RX)     │  White wire
│ GP1 (UART RX)├────────────┤ G32 (TX)     │  Yellow wire  
│ GND          ├────────────┤ GND          │  Black wire
│ VBUS (5V USB)│            │ 5V           │  (optional — separate USB power)
└──────────────┘            └──────────────┘
                             G26 (HAT) → 74AHCT125 → LED strip
```

3 wires total (TX, RX, GND). Pico powered by its own USB cable from PC.
M5StickC powered by its own USB cable (or battery).

### Architecture (chosen approach)

```
Host PC ←─ USB-C CDC-NCM ─→ Pi Pico RP2040 ←─ UART 3Mbps ─→ M5StickC ESP32
               12 Mbps        │ TinyUSB NCM      │ PPP/eppp      │ esp_netif
               zero-config    │ lwIP bridge       │ G32/G33 Grove │ WLED
               Ethernet       │ PPP client        │               │ AsyncWebSvr
                               └─ ~300 lines FW    └─ existing PPP │ DDP/E1.31
                                                     code works!   │ Effects
                                                                   │ G26 → LEDs
```

### Implementation: Pico Firmware

Two options for the Pico side:

**Option 1: CDC-NCM → PPP bridge (simplest, ESP32 unchanged)**
- TinyUSB presents CDC-NCM to host
- lwIP on Pico handles IP routing
- PPP client over UART to ESP32
- ESP32 runs existing PPP server code — ZERO CHANGES

**Option 2: CDC-NCM → raw frame bridge (fastest, minimal Pico code)**
- TinyUSB presents CDC-NCM to host
- Raw Ethernet frames forwarded over UART with length-prefix framing
- ESP32 needs custom esp_netif driver to unwrap frames
- Slightly more ESP32 work but eliminates PPP overhead

**Option 1 is recommended** — our ESP32 PPP code is already written and tested.
The Pico just needs to be a "smart FTDI" that speaks Ethernet on one side and PPP on the other.
