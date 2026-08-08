# Iteration Wave 3: Build Optimization + ARGB Passthrough

## Context

Wave 2 delivered PPP netif implementation (59.7% flash, 746KB headroom). Hardware testing blocked (no M5StickC). This wave focuses on software-only work before first flash.

Current firmware: m5stickc_ppp at 1,135,792 bytes (59.7% of 1.81MB partition).
All builds on koero NVMe at /var/mnt/koero/workspace/wled/.

**Retained features** (NOT trimmable): effects engine, particle systems 1D/2D, pixelforge, filesystem, WebSocket. These are core to the WLED experience.

**Optimization target**: sdkconfig-level radio/compiler changes — WiFi and BT stacks are the biggest flash/RAM consumers and we use neither (PPP runs on UART, BLE is backlog).

## TODOs

- [ ] 1. sdkconfig radio disable — add CONFIG_ESP_WIFI_ENABLED=n and CONFIG_BT_ENABLED=n to sdkconfig.defaults, add WLED_DISABLE_IMPROV_WIFISCAN build flag, rebuild m5stickc_ppp, handle any WiFi.h compile breaks with ifdef guards, measure flash/RAM savings
- [ ] 2. sdkconfig compiler + bootloader optimization — add CONFIG_COMPILER_OPTIMIZATION_SIZE=y, CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_SIZE=y, CONFIG_LOG_DEFAULT_LEVEL_WARN=y, rebuild, measure cumulative savings vs Wave 2 baseline (1,135,792B)
- [ ] 3. ARGB passthrough RMT RX — create wled_argb_passthrough.h/.cpp: RMT RX on configurable GPIO (default G32), decode WS2812B pulses into pixel colors, feed setRealtimePixel() with new REALTIME_MODE_ARGB_PASSTHROUGH (const.h value 10), build and verify
- [ ] 4. Boot state machine — wire passthrough into PPP lifecycle: on boot start ARGB passthrough (realtimeLock), on IP_EVENT_PPP_GOT_IP call stopARGBPassthrough/exitRealtime, on IP_EVENT_PPP_LOST_IP re-engage passthrough. Add to wled_ppp.cpp event handler (~7 lines)
- [ ] 5. TFT status display usermod — create usermods/m5stickc_ppp/ with basic ST7735S display: IP/PPP status, current mode (passthrough vs WLED), effect name, brightness. Use existing SPI pins (G15/G13/G5/G23/G18)
- [ ] 6. Full build verification + size report — clean build all envs (m5stickc, m5stickc_lean, m5stickc_ppp), size comparison table across Wave 1/2/3, commit everything with updated AGENTS.md + learnings.md

## Final Verification Wave

- [ ] F1. Clean pio run -e m5stickc_ppp succeeds with passthrough + display usermod
- [ ] F2. Binary fits 4MB flash with all Wave 3 features (target: under 1MB if radio stacks removed)
- [ ] F3. No regressions: m5stickc and m5stickc_lean still build clean
- [ ] F4. All new code is ifdef guarded — non-PPP builds unaffected

## Estimated Savings (sdkconfig-level, to verify in T1-T2)

| Optimization | Est. Flash Saved | Est. RAM Saved | Notes |
|---|---|---|---|
| CONFIG_ESP_WIFI_ENABLED=n | ~80-120KB | ~40KB | WiFi driver + stack + radio firmware |
| CONFIG_BT_ENABLED=n | ~60-100KB | ~30KB | BT controller + host stack (re-enable for BLE later) |
| WLED_DISABLE_IMPROV_WIFISCAN | ~3KB | ~1KB | WiFi scan for provisioning (useless without WiFi) |
| CONFIG_COMPILER_OPTIMIZATION_SIZE | ~20-40KB | ~0 | -Os vs default optimization |
| CONFIG_LOG_DEFAULT_LEVEL_WARN | ~5-10KB | ~0 | Remove debug/info log strings |
| **Total estimated** | **~170-270KB** | **~71KB** | |
| **Current** | 1,135,792B (59.7%) | 81,152B (24.8%) | |
| **Target** | ~870-970KB (~46-51%) | ~50-60KB (~15-18%) | |

## ARGB Passthrough Design (T3-T4)

Reuses WLED existing patterns with minimal new code:

| WLED Function | Our Use |
|---|---|
| realtimeLock(timeout, mode) | Lock strip for motherboard passthrough |
| setRealtimePixel(i, r, g, b, w) | Set LED from RMT RX captured data |
| exitRealtime() | Release when PPP connects |
| realtimeMode = 10 | New REALTIME_MODE_ARGB_PASSTHROUGH |

New files: wled_argb_passthrough.h (~30 lines), wled_argb_passthrough.cpp (~100 lines)
Modified: const.h (+1 line), wled.cpp (+5 lines), wled_ppp.cpp (+7 lines)
See refs/argb-passthrough.md for full architecture.

## After Wave 3

Hardware test is the sole remaining blocker:
1. Flash m5stickc_ppp to M5StickC via USB
2. Test PPP: pppd -> wled.local -> dashboard
3. Test ARGB passthrough: motherboard header -> G32 -> LEDs mirror
4. Test state transition: passthrough -> PPP control -> passthrough
5. Test DDP: OpenRGB -> wled.local:4048 -> LEDs

## Success Criteria

- Firmware significantly smaller than Wave 2 baseline (target: under 1MB)
- ARGB passthrough compiles and integrates with WLED realtime mode
- Boot state machine: passthrough on boot, WLED on PPP connect
- TFT status display shows mode/IP/effect
- All ifdef guarded, non-PPP builds unaffected
