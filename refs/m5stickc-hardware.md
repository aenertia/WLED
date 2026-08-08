# M5StickC (Original, K016-C) — Hardware Reference

## SoC: ESP32-PICO-D4

| Parameter | Value |
|---|---|
| Core | Xtensa 32-bit LX6 dual-core |
| Clock | Up to 240 MHz |
| Flash | 4 MB (integrated in SiP) |
| PSRAM | **None** |
| SRAM | 520 KB |
| Wi-Fi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | BT 4.2 + BLE |

Free heap after FreeRTOS + Arduino framework: ~200–280 KB. A 300-LED RGBW strip needs ~1.2 KB for pixel data — not a constraint.

## USB Interface: FTDI FT232

**Not** CP2104 (some ESP32 devkits), **not** CH9102 (M5StickC Plus2).

The official M5Stack docs direct users to install the [FTDI VCP driver](https://ftdichip.com/drivers/vcp-drivers/).

### Supported Baud Rates

| Baud | Status |
|---|---|
| 115200 | Standard, reliable |
| 250000 | Supported |
| 500000 | Supported |
| 750000 | Supported |
| **1500000** | **Maximum supported** |
| 921600 | NOT in official list — avoid |
| 2000000+ | Not supported |

### Throughput at Max Baud (1.5 Mbps)

~187.5 KB/s raw. A 300-LED RGB Adalight frame = 906 bytes → theoretical ~207 fps at serial layer (LED refresh timing is the real bottleneck). At 115200 baud: ~12 fps for 300 LEDs (marginal). **500K–1.5M recommended**.

## GPIO Pinout — Internal Peripherals (OCCUPIED)

| Peripheral | GPIO(s) | Notes |
|---|---|---|
| TFT LCD (ST7735S, 80×160) | G15 (MOSI), G13 (CLK), G23 (DC), G18 (RST), G5 (CS) | SPI bus, all 5 consumed |
| Red LED | G10 | Active low |
| IR Transmitter | G9 | TX only |
| Button A | G37 | Active low, has pull-up |
| Button B (Power/RST) | G39 | Active low |
| IMU (MPU6886) + RTC (BM8563) + PMU (AXP192) | G21 (SDA), G22 (SCL) | Shared I2C; AXP192 IRQ on G35 |
| Microphone (SPM1423) | G0 (CLK), G34 (DATA) | I2S PDM |

## GPIO Pinout — Available for User

| GPIO | Connector | Capabilities | LED Data? |
|---|---|---|---|
| G0 | HAT header | Strapping pin + mic CLK conflict | **Avoid** |
| **G26** | HAT header | DAC2, ADC2_CH9, **RMT-capable** | **Best choice** |
| G36 | HAT header | ADC1_CH0, **input-only** | No — cannot output |
| **G32** | Grove (Yellow) | ADC1_CH4, Touch9, **RMT-capable** | Good alternative |
| **G33** | Grove (White) | ADC1_CH5, Touch8, **RMT-capable** | Good alternative |

## Connectors

### Grove Port (HY2.0-4P)

| Pin | Color | Signal | Voltage |
|---|---|---|---|
| 1 | Black | GND | — |
| 2 | Red | 5V (IPSOUT via AXP192) | 5V |
| 3 | Yellow | G32 | 3.3V logic |
| 4 | White | G33 | 3.3V logic |

### HAT Pin Header (8-pin)

G0, G26, G36, 5V, GND, 3.3V, BAT, GND

## ESP32 RMT Peripheral

| Parameter | Value |
|---|---|
| Total channels | 8 (channels 0–7) |
| TX-capable | All 8 |
| RX-capable | All 8 |
| Memory/channel | 64 × 32-bit words (256 bytes) |
| Supported GPIOs | **Any output-capable GPIO** (via GPIO matrix) |
| DMA on ESP32 | **Not available** (ESP32-S3/C6 only) |
| Clock source | APB 80 MHz |

RMT works on any output-capable GPIO through the GPIO matrix. GPIO36 is input-only. For WS2812B: 1 RMT TX channel, 10 MHz resolution (100ns tick), ping-pong mode for streaming.

## 3.3V vs 5V Logic Level — WS2812B

### WS2812B Datasheet Thresholds (at VDD = 5V)

| Parameter | Value |
|---|---|
| V_IH (input high min) | 0.7 × VDD = **3.5V** |
| V_IL (input low max) | 0.3 × VDD = **1.5V** |

**ESP32 outputs 3.3V HIGH — below the 3.5V V_IH spec.** Out of spec.

### Practical Reality

- Short runs (<10 LEDs, short wires): often works due to manufacturing margin
- Long runs (50+ LEDs): **unreliable** — glitches, flickering, wrong colors
- Cold temperatures: worse (thresholds tighten)

### Solutions (ranked)

1. **74AHCT125 or 74HCT245 level shifter** — gold standard, single chip, 3.3V input accepted as logic high (HCT threshold ~1.4V). **Strongly recommended.**
2. Sacrificial first LED at 3.3V VDD — unreliable hack
3. Diode voltage drop (1N4148 in 5V line → ~4.3V VDD, V_IH = 3.01V) — wastes power
4. 3.3V-tolerant LED chips (SK6805-EC15/EC20, some APA106)

## Power

### AXP192 PMU Rails

| Rail | Voltage | Limit | Notes |
|---|---|---|---|
| 5V out (Grove, HAT) | ~5V (USB) | ~500 mA total input | Shared with battery charging |
| 3.3V out | 3.3V | Powers ESP32 + IMU | Not exposed externally |
| BAT (HAT) | 3.7V nom | 95 mAh cell | Tiny |

**USB input is 500 mA total.** After ESP32 (~80 mA), display (~20 mA), and battery charging, there is **very little headroom** for external LEDs via the onboard 5V pin.

### LED Power Budget

| LEDs | Current @ full white | Power |
|---|---|---|
| 10 | ~600 mA | 3W |
| 30 | ~1.8 A | 9W |
| 60 | ~3.6 A | 18W |

**External 5V PSU is mandatory for LED strips.** Share only GND between M5StickC and LED power supply.

## Known Limitations

1. **GPIO0 is a strapping pin** — pulling low at boot enters download mode. Never use for LED data.
2. **GPIO36 is input-only** — useless for RMT output.
3. **No DMA for RMT on ESP32** — CPU must service RMT interrupts to refill 64-symbol buffer. Creates brief CPU load spikes during LED updates.
4. **WiFi + RMT interrupt contention** — WiFi interrupts can delay RMT refills on long chains (100+ LEDs). Mitigations: pin RMT to core 1, WiFi to core 0; use `ESP_INTR_FLAG_IRAM`.
5. **95 mAh battery** — ~20-30 min active with WiFi. Irrelevant for USB-tethered use.
6. **Screen uses 5 SPI GPIOs** — if display is disabled in firmware, pins can be reclaimed.

## Recommended Pin Assignment

```
LED Data Out:     G26 (HAT header) — via 74AHCT125 level shifter
USB Serial:       FTDI FT232 (internal) — up to 1.5 Mbps
Button A:         G37 (mode/brightness)
Button B:         G39 (power — 6s hold = off)
I2C (optional):   G21/G22 (shared with IMU)
```

## Reference Links

| Resource | URL |
|---|---|
| Official Product Page | https://docs.m5stack.com/en/core/m5stickc |
| Schematic PDF | https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/schematic/Core/M5StickC/20191118__StickC_A04_3110_Schematic_Rebuild_PinMap.pdf |
| ESP32-PICO-D4 Datasheet | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/669/esp32-pico_series_datasheet_en.pdf |
| AXP192 Datasheet | https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/AXP192_datasheet_en.pdf |
| Arduino Library | https://github.com/m5stack/M5StickC |
| ESP-IDF RMT Docs | https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/rmt.html |
