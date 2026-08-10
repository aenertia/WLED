# PR Reference: PPP-over-Serial Transport

## Status: No existing issue — CREATE FEATURE REQUEST FIRST

No prior issues, PRs, or discussions exist for PPP/SLIP/IP-over-serial in WLED.
Search terms yielding zero results: PPP, pppd, SLIP, CDC-ECM, CDC-NCM, USB network,
IP over serial, serial networking, USB tethered.

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
| [#5650](https://github.com/wled/WLED/pull/5650) | Independent Ethernet/WiFi IP config + primary netif selection | Open (v17.0) | lwIP netif infrastructure for multiple interfaces — PPP registers as another netif |
| [#5697](https://github.com/wled/WLED/pull/5697) | ESP32-P4 32MB support (eth&DDP only) | Closed | Introduces WLED_ETHERNET_ONLY_BUILD — template for WLED_PPP_ONLY_BUILD |
| [#5667](https://github.com/wled/WLED/pull/5667) | RTL8201 wESP32 rev7 Ethernet | Open (v16.1) | Pattern for adding new network hardware |

## Serial Port Conflicts (must address in PR)

| Issue | Title | Status | Impact |
|-------|-------|--------|--------|
| [#5652](https://github.com/wled/WLED/issues/5652) | Serial RX noise — option to disable serial port | Open (v16.1) | PPP must claim serial exclusively; Adalight must be disabled |
| [#4662](https://github.com/wled/WLED/issues/4662) | USB_CDC + WLED_DEBUG blocks UART TX pin | Closed | USB-CDC vs UART pin conflict on S3/C3 |
| [#5501](https://github.com/wled/WLED/issues/5501) | No ADAlight response on ESP32 | Closed | Adalight serial handling is fragile — PPP must not break it |

## Suggested Issue Template

Title: `Feature Request: PPP over Serial — IP networking via USB/UART without WiFi`

Body: Reference #4990, #1382, #5697. Propose WLED_USE_PPP flag following the
WLED_USE_ETHERNET pattern. ESP-IDF native esp_netif PPP, UART RX task, IPCP
address assignment. ~350 lines core change. Use case: USB-tethered LED controller
(PC ARGB, kiosk, art installation) where WiFi is unavailable or undesirable.

## Key Technical Notes for PR Description

- Follows `WLED_USE_ETHERNET` pattern exactly: `isPPP()` alongside `isEthernet()`
- PR #5650 `setPrimaryNetworkInterface()` / `netif_set_default()` pattern reusable
- PR #5697 `WLED_ETHERNET_ONLY_BUILD` early-return pattern directly applicable
- ESP-IDF PPP is first-class (`esp_netif` PPP driver, used in every cellular project)
- Zero WiFi code changes — PPP is purely additive (`|| isPPP()` guards)
- Serial ownership: must disable Adalight when PPP active (address #5652)

