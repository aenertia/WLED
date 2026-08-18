# ARGB Motherboard Header Passthrough via RMT

**Forgejo**: Fixes #13

## Summary

Adds ARGB passthrough mode for PC motherboard 3-pin ARGB headers. The ESP32 RMT peripheral captures the incoming WS2812B signal and re-transmits it to the output pin, allowing the M5StickC to act as a transparent ARGB bridge.

## Use Case

Capture ARGB signals from motherboard software (ASUS Aura, MSI Mystic Light, etc.) and forward them to additional LED strips, while simultaneously displaying the captured pattern on the WLED matrix.

## Changes

| File | Description |
|------|-------------|
| `wled_argb_passthrough.cpp` | RMT RX capture + TX retransmit engine |
| `wled_argb_passthrough.h` | Public API: init, start/stop, handle, status queries |
| `wled_argb_capture.h` | Signal capture utilities and timing constants |
| `wled.cpp` | Loop hook (`handleARGBPassthrough`) + setup init |
| `wled.h` | Conditional include under `WLED_ENABLE_ARGB_PASSTHROUGH` |
| `json.cpp` | ARGB status block in `/json/info` response |

## Build Flag

```
-D WLED_ENABLE_ARGB_PASSTHROUGH
```

## Hardware Requirements

- ESP32 with RMT peripheral (ESP32, ESP32-S2, ESP32-S3)
- One GPIO for RMT RX (ARGB input from motherboard header)
- One GPIO for RMT TX (output to LED strip)

## Testing

- Tested on M5StickC (ESP32-PICO-D4) capturing WS2812B signal from ASUS Aura header
- Verified passthrough latency < 1 frame at 30 FPS
- `/json/info` reports capture status, LED count, and segment count

## Notes

- Build flag is opt-in; no impact on standard WLED builds
- No dependencies on other fork features (PPP, TFT, etc.)
## Related upstream issues

| Issue/PR | Repo | Title | Relevance |
|----------|------|-------|-----------|
| [#2675](https://github.com/wled/WLED/issues/2675) | Aircoookie/WLED | Gen2 ARGB multi-layer mode (open, backburner) | Long-standing request for ARGB signal relay/passthrough |
| [#1116](https://github.com/wled/WLED/issues/1116) | Aircoookie/WLED | MSI Mystic Light support (open, 6+ years) | Oldest open request for PC ARGB integration — this PR addresses it |
