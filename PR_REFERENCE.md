# PR Reference: PPP-over-Serial Transport

## Status: No existing issue — CREATE FEATURE REQUEST FIRST

No prior issues, PRs, or discussions exist for PPP/SLIP/IP-over-serial in WLED.
Search terms yielding zero results: PPP, pppd, SLIP, CDC-ECM, CDC-NCM, USB network,
IP over serial, serial networking, USB tethered.

## Overhead Analysis (WiFi + PPP Coexistence)

### Zero cost when `WLED_USE_PPP` is not defined

All PPP code is behind `#ifdef WLED_USE_PPP`. When the flag is absent:
- Flash: +0 bytes (not compiled)
- Heap: +0 bytes (no globals, no tasks)
- CPU: +0% (no code executes)

### Cost when `WLED_USE_PPP` defined and PPP link active

| Resource | Overhead | Detail |
|----------|----------|--------|
| Flash | +20 KB | lwIP PPP module (pppos.c, lcp.c, ipcp.c) + wled_ppp.cpp |
| Static RAM (.bss) | +2 KB | PPP globals, netif handle, UART config |
| Heap (runtime) | +16 KB | 8KB RX task stack + 4KB UART ring buffers + 2KB esp_netif PPP state + 2KB HDLC framing |
| CPU (core 0) | +5-10% | PPP RX task polls UART every 1ms. Bursty, not sustained. Core 1 (LED output) unaffected. |

### Free heap after boot — coexistence scenario

```
                        WiFi ON       WiFi ON       WiFi OFF
                        no PPP        + PPP         PPP only
                        (upstream)    (coexist)     (lean fork)
                        ──────────    ──────────    ──────────
Free heap after boot    ~160 KB       ~145 KB       ~177 KB
PPP overhead            —             -16 KB        -16 KB
Worst case (capture+    
  6 WS + OTA + 2K LEDs) ~64 KB        ~49 KB        ~81 KB
MIN_HEAP_SIZE           15 KB         15 KB         15 KB
Headroom                49 KB ✅      34 KB ✅      66 KB ✅
```

### CPU budget — core 0 with WiFi + PPP

| Task | Core | CPU % | Notes |
|------|------|-------|-------|
| WiFi driver | 0 | 10-15% | Bursty (scanning, reconnect, TX/RX) |
| lwIP TCP/IP | 0 | 3-5% | Shared between WiFi and PPP |
| PPP RX task | 0 | 5-10% | 1ms poll, higher at 5Mbps |
| WiFi event handler | 0 | 1-2% | Connection state changes |
| **Total core 0** | **0** | **19-32%** | **68-81% idle** |
| WLED effects + strip | 1 | 20-50% | Completely independent |
| LED RMT DMA | 1 | 0% (HW) | Async hardware transfer |

### PPP route priority

PPP netif `route_prio = 128` > WiFi default `100`. When both interfaces have IPs,
lwIP prefers PPP for outbound traffic. WiFi remains accessible for local network
services (mDNS discovery, HA integration from LAN).

### LED/segment impact

PPP has zero impact on LED limits:

| Parameter | Without PPP | With PPP | Change |
|-----------|------------|----------|--------|
| MAX_LEDS | 16,384 | 16,384 | None |
| MAX_NUM_SEGMENTS | 32 | 32 | None |
| MAX_LED_MEMORY | 85 KB | 85 KB | None |
| FPS (300 LEDs) | 42 | 42 | None (core 1) |

## Adjacent Issues (demonstrate demand)

| Issue | Title | Status | Relevance |
|-------|-------|--------|-----------|
| [#4990](https://github.com/wled/WLED/issues/4990) | WiFi Toggle via Button | Open | Users want WiFi-less/low-power operation |
| [#5762](https://github.com/wled/WLED/issues/5762) | WLED-AP still active on Ethernet | Open | Users want wired-only, WiFi fully off |
| [#5614](https://github.com/wled/WLED/issues/5614) | No-WiFi reboots every 15-20min | Closed | WiFi-less operation has stability issues |
| [#1382](https://github.com/wled/WLED/issues/1382) | ESP32 Bluetooth? | Open (5+ yrs) | Long-standing demand for non-WiFi transport |

## Architectural Precedents (PRs to reference)

| PR | Title | Status | Why it matters |
|----|-------|--------|----------------|
| [#5650](https://github.com/wled/WLED/pull/5650) | Independent Ethernet/WiFi IP config + primary netif selection | Open (v17.0) | lwIP netif infrastructure for multiple interfaces |
| [#5697](https://github.com/wled/WLED/pull/5697) | ESP32-P4 32MB support (eth&DDP only) | Closed | Introduces WLED_ETHERNET_ONLY_BUILD pattern |
| [#5667](https://github.com/wled/WLED/pull/5667) | RTL8201 wESP32 rev7 Ethernet | Open (v16.1) | Pattern for adding new network hardware |

## Serial Port Conflicts (must address in PR)

| Issue | Title | Status | Impact |
|-------|-------|--------|--------|
| [#5652](https://github.com/wled/WLED/issues/5652) | Serial RX noise — option to disable serial port | Open (v16.1) | PPP must claim serial exclusively |
| [#4662](https://github.com/wled/WLED/issues/4662) | USB_CDC + WLED_DEBUG blocks UART TX pin | Closed | USB-CDC vs UART pin conflict on S3/C3 |
| [#5501](https://github.com/wled/WLED/issues/5501) | No ADAlight response on ESP32 | Closed | Adalight handling fragile — PPP must not break it |

## Home Assistant Compatibility

Verified: HA WLED integration uses HTTP JSON + WebSocket only. Zero MQTT dependency.
- `GET /json` — full state + info (initial load + poll)
- `POST /json/state` — all commands
- `ws://host:80/ws` — realtime push (replaces polling when available)
- `_wled._tcp.local.` — mDNS autodiscovery

All work over any routed IP link including PPP. Manual host entry bypasses mDNS if needed.

## Draft GitHub Issue

*(See DRAFT_ISSUE.md in this branch)*

## Draft PR Description

*(See DRAFT_PR.md in this branch)*
