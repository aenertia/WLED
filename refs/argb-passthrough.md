# ARGB Motherboard Passthrough — Architecture Reference

## Concept

On boot, the M5StickC captures incoming WS2812B data from the motherboard ARGB
header via RMT RX on a GPIO pin. The captured pixel data feeds into WLED's existing
`setRealtimePixel()` function, making the LEDs mirror the motherboard output.
When USB PPP control is established, WLED releases the realtime lock and takes over.

## Why This Matters for PC Use

During PC boot (POST), the motherboard drives ARGB LEDs with status colors:
- Red: error / hardware fault
- Orange/amber: memory training
- White cycling: initializing
- Rainbow: fully booted (default effect)

The user sees familiar boot behavior until the OS loads and pppd connects.
No "dead LEDs" during POST — the controller is transparent until needed.

## Reuse of WLED Patterns (minimal new code)

| WLED Function | Existing Use | Our Use |
|---|---|---|
| `realtimeLock(timeout, mode)` | DDP/E1.31/Adalight lock strip for external data | Lock strip for motherboard passthrough |
| `setRealtimePixel(i, r, g, b, w)` | Set individual LED from UDP pixel data | Set individual LED from RMT RX capture |
| `exitRealtime()` | Release lock, resume WLED effects | Release lock when PPP connects |
| `realtimeMode` global | UI shows current realtime source | Shows "ARGB Passthrough" in dashboard |
| `realtimeTimeout` | Auto-exit after timeout | Long timeout (or disabled) for passthrough |

New constant in const.h:
```cpp
#define REALTIME_MODE_ARGB_PASSTHROUGH 10  // next after DMX=9
```

## RMT RX for WS2812B Capture

### How it works

ESP32 RMT peripheral in RX mode measures pulse durations on a GPIO pin.
WS2812B encoding:
- Bit 0: ~0.4us HIGH, ~0.85us LOW (total ~1.25us)
- Bit 1: ~0.8us HIGH, ~0.45us LOW (total ~1.25us)
- Reset: >280us LOW

RMT captures these as (duration, level) pairs. Firmware decodes
pulse widths into GRB color values.

### Memory constraints

- RMT memory: 64 symbols (32-bit each) per channel
- Each WS2812B bit = 2 RMT symbols (high pulse + low pulse)
- Each LED = 24 bits = 48 RMT symbols
- 64 symbols = ~1.3 LEDs per memory block

**Solution**: Use multiple RMT memory blocks (up to 8 channels worth = 512 symbols)
or continuous RX mode with interrupt-driven buffer. ESP-IDF 5.x provides
`rmt_rx_register_event_callbacks()` for non-blocking continuous capture.

### Practical LED count limits

| Approach | Max LEDs | RAM | CPU Load |
|---|---|---|---|
| Single RMT block (64 sym) | ~1 | 256B | Minimal |
| 4 RMT blocks (256 sym) | ~5 | 1KB | Low |
| Continuous RX + ring buffer | ~200 | ~4KB | Moderate (interrupt-driven) |
| SPI slave oversample (2.5MHz) | ~500 | ~8KB | Low (DMA) |

**Recommended**: Continuous RX with ring buffer. Target 60-100 LEDs (typical PC case).

### GPIO assignment

- **G32** (Grove yellow): RMT RX input — receives motherboard ARGB data
- **G26** (HAT header): RMT TX output — drives LED strip (existing)
- Independent RMT channels, no conflict

### Voltage consideration

Motherboard ARGB header outputs 5V data. ESP32 GPIO is 3.3V tolerant but
rated for 3.3V input. However, ESP32 GPIOs are actually 5V tolerant in practice
(input threshold ~2.5V, absolute max ~3.6V per datasheet but commonly
used at 5V without damage). A voltage divider (10K + 20K) or level
shifter is recommended for reliability.

## Boot State Machine

```
                    +─────────────────+
                    │   BOOT          │
                    │ (ESP32 starts)  │
                    +────────┬────────+
                             │
                    ┌────────▼────────┐
                    │ PASSTHROUGH     │ RMT RX → setRealtimePixel() → LEDs
                    │ realtimeLock()  │ Motherboard controls LEDs
                    │ mode=10        │ PPP server listening on UART0
                    └────────┬────────┘
                             │ IP_EVENT_PPP_GOT_IP
                    ┌────────▼────────┐
                    │ WLED CONTROL    │ Effects/DDP/API active
                    │ exitRealtime()  │ Dashboard at wled.local
                    │ mode=0         │ OpenRGB DDP on :4048
                    └────────┬────────┘
                             │ IP_EVENT_PPP_LOST_IP
                    ┌────────▼────────┐
                    │ PASSTHROUGH     │ Back to motherboard mirror
                    │ realtimeLock()  │ Until PPP reconnects
                    └─────────────────┘
```

## Implementation — Minimal Code

### wled_argb_passthrough.h (~30 lines)

```cpp
#pragma once
#ifdef WLED_ENABLE_ARGB_PASSTHROUGH

#include "driver/rmt_rx.h"

#ifndef ARGB_RX_PIN
#define ARGB_RX_PIN 32  // Grove yellow
#endif
#ifndef ARGB_MAX_LEDS
#define ARGB_MAX_LEDS 100
#endif

void initARGBPassthrough();
void handleARGBPassthrough();  // call from loop()
void startARGBPassthrough();   // engage passthrough mode
void stopARGBPassthrough();    // release to WLED control

#endif
```

### wled_argb_passthrough.cpp (~100 lines)

Core logic:
1. `initARGBPassthrough()`: Configure RMT RX channel on ARGB_RX_PIN
2. `handleARGBPassthrough()`: In main loop, check for new RMT data, decode WS2812B pulses, call `setRealtimePixel()` for each LED
3. `startARGBPassthrough()`: Call `realtimeLock(UINT32_MAX, REALTIME_MODE_ARGB_PASSTHROUGH)`
4. `stopARGBPassthrough()`: Call `exitRealtime()`

### Integration in wled_ppp.cpp (3 lines)

In the PPP event handler:
```cpp
if (event_id == IP_EVENT_PPP_GOT_IP) {
    ppp_connected = true;
    #ifdef WLED_ENABLE_ARGB_PASSTHROUGH
    stopARGBPassthrough();  // ← release to WLED
    #endif
}
if (event_id == IP_EVENT_PPP_LOST_IP) {
    ppp_connected = false;
    #ifdef WLED_ENABLE_ARGB_PASSTHROUGH
    startARGBPassthrough(); // ← back to motherboard mirror
    #endif
}
```

### Integration in wled.cpp (2 lines)

In setup(), after initPPP():
```cpp
#ifdef WLED_ENABLE_ARGB_PASSTHROUGH
initARGBPassthrough();
startARGBPassthrough();  // start in passthrough mode
#endif
```

In loop():
```cpp
#ifdef WLED_ENABLE_ARGB_PASSTHROUGH
handleARGBPassthrough();
#endif
```

**Total new code**: ~130 lines in 2 new files + ~7 lines in existing files.
**Total existing code reused**: realtimeLock(), setRealtimePixel(), exitRealtime(), realtimeMode — zero modification.

## Hardware Mux Alternative (even simpler)

For zero-firmware passthrough, use a 74HC4053 analog multiplexer:
- Input A: Motherboard ARGB data
- Input B: ESP32 G26 (WLED output)
- Output: LED strip DIN
- Select: ESP32 GPIO (LOW=mobo, HIGH=ESP32)

Set select GPIO HIGH in the PPP got-IP handler. Set LOW on lost-IP.
2 lines of code total. No RMT RX needed. But loses the ability to
READ/ANALYZE the motherboard data (POST state detection).
