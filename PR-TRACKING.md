# PR Tracking -- WLED Upstream Contributions

**Fork**: [aenertia/WLED](https://github.com/aenertia/WLED)
**Forgejo**: https://git.awa.3d.ae.net.nz/aenertia/wled
**Branch base**: All pr/* branches are on `upstream/main`
**Upstream**: [Aircoookie/WLED](https://github.com/Aircoookie/WLED)
**Active branch**: `dev/ppp-wifi`
**Last updated**: Session 28 (August 2026)

## Status summary

| Metric | Value |
|--------|-------|
| PR branches | 20 local + upstream-pr/segment-eligibility |
| Upstream PRs | 3 open, 1 merged, 2 closed |
| Upstream issues | 2 open, 1 closed |
| ESPAsync discussion | 1 filed |
| Device soak test | 250min+ continuous, reset=1 (POWERON) |

## Active upstream PRs

| PR | Branch | Description | Status |
|----|--------|-------------|--------|
| [#5804](https://github.com/wled/WLED/pull/5804) | `pr/watchdog-idf5-compat` | esp_task_wdt_config_t struct API for IDF 5.x | Open |
| [#5806](https://github.com/wled/WLED/pull/5806) | `pr/ws-state-only-broadcast` | Skip serializeInfo() on WS broadcast -- saves 4-6KB heap | Open -- CR feedback addressed |
| [#5807](https://github.com/wled/WLED/pull/5807) | `pr/audioreactive-pdm-fix` | Skip i2s_set_clk() for PDM mode on IDF 5.x | Open |

## Active upstream issues

| Issue | Description | Status |
|-------|-------------|--------|
| [#5810](https://github.com/wled/WLED/issues/5810) | DDP compression extension -- gauging interest | Open -- spec posted, feedback received |
| [#5811](https://github.com/wled/WLED/issues/5811) | PPP network transport for ESP32 | Open |
| [ESP32Async #472](https://github.com/ESP32Async/ESPAsyncWebServer/discussions/472) | AsyncAbstractResponse RESPONSE_WAIT_ACK exits before ACKs on chunked path | Filed -- Forgejo [#28](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/28) |

## Ready to submit

### Per-segment realtime eligibility (next up)

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/segment-eligibility` | [#26](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/26) | Per-segment realtime eligibility mask (Mode B only, replaces useMainSegmentOnly) | Ready -- pushed to GitHub, merged into dev/ppp-wifi, validated on hardware |
| `upstream-pr/segment-eligibility` | -- | Submission branch (pr/segment-eligibility sans DRAFT_PR.md) | Ready for manual PR submission |

Supersedes `pr/ddp-per-segment` for upstream. Mode A (destination-byte routing) excluded per softhack007 feedback ("separate PR"). `pr/ddp-per-segment` retained fork-local for Mode A + Mode B combined.

### Bug fixes

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/mdns-ppp-crash-fix` | [#18](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/18) | mDNS NULL netif crash on WiFi STA disconnect under PPP | Ready |

### DDP compression

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/ddp-rle-codec` | [#6](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/6) | Header-only RLE codec (ddp_compress.h) | Ready |
| `pr/ddp-compressed-receiver` | [#7](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/7) | Compressed DDP decode in handleDDPPacket() -- depends on ddp-rle-codec | Ready |
| `pr/ddp-compressed` | [#16](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/16) | Full stack: codec + receiver + tools + spec | Ready |

### Effects

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/effects-deferred-fade` | [#10](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/10) | Deferred fade accumulator | INCOMPLETE -- V2 broke scrolling text |

### Text

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/text-aa-fonts` | [#11](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/11) | DejaVu Bold 18px + 40px 4bpp anti-aliased fonts | Ready |
| `pr/text-drop-shadow` | [#12](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/12) | Drop shadow with angle/distance/intensity | Stub -- implementation pending |

### Hardware / transport

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/argb-passthrough` | [#13](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/13) | ARGB motherboard header passthrough via RMT | Ready |
| `pr/tft-bus-matrix` | [#14](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/14) | BusSPIMatrix -- SPI display as pixel matrix output bus | Ready -- depends on pr/bus-skip-show |
| `pr/ppp-transport` | [#15](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/15) | PPP-over-serial network transport (WLED_USE_PPP) | Ready -- issue [#5811](https://github.com/wled/WLED/issues/5811) open |
| `pr/slip-transport` | [#8](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/8) | SLIP transport -- low priority | Ready |

### Performance

| Branch | Forgejo | Description | Status |
|--------|---------|-------------|--------|
| `pr/bus-skip-show` | [#22](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/22) | Generalized idle-skip gate -- skip show() for buses with no active segments | Ready |
| `pr/ddp-per-segment` | [#26](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/26) | Dual-mode DDP routing (Mode A + Mode B combined) | Fork-local -- upstream submission via pr/segment-eligibility (Mode B only) |

### Upstream component fixes (target: arduino-esp32 / esp-idf)

| Branch | Forgejo | Target repo | Description | Status |
|--------|---------|-------------|-------------|--------|
| `pr/arduino-esp32-mdns-guard` | [#19](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/19) | espressif/arduino-esp32 | ESPmDNS::end() NULL deref guard | Ready |
| `pr/arduino-esp32-netif-lazy-init` | [#20](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/20) | espressif/arduino-esp32 | esp_netif_init() before netif creation | Ready |
| `pr/esp-idf-lcp-echo-docs` | [#21](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/21) | espressif/esp-idf | LWIP_ENABLE_LCP_ECHO Kconfig warning | Ready |

### Deferred

| Branch | Forgejo | Description | Reason |
|--------|---------|-------------|--------|
| `pr/ddp-flood-hardening` | [#27](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/27) | Heap guard + rate limiter + starvation detector | Interleaved with ddp-per-segment in e131.cpp; defer until segment-eligibility lands upstream |

## Closed / merged upstream

| PR | Branch | Description | Outcome |
|----|--------|-------------|---------|
| [#5805](https://github.com/wled/WLED/pull/5805) | `pr/segment-name-race-fix` | alloc-fill-swap-free in Segment::setName() -- dual-core race fix | **Merged** -- comments stripped per reviewer feedback (190676d3) |
| [#5808](https://github.com/wled/WLED/pull/5808) | `pr/chunked-json-fix` | Content-Length for /json/fxdata -- prevents truncation on slow links | **Closed** -- root cause found: sendChunked returned 0 instead of RESPONSE_TRY_AGAIN. Content-Length approach vetoed by willmmiles. Fix in json_chunked.h on dev/ppp-wifi |
| [#5809](https://github.com/wled/WLED/pull/5809) | `pr/effects-fade-snap` | Snap-to-target in fade_out/fadeToBlackBy | **Closed** -- maintainer rejected (changes visual behaviour); kept fork-local |
| [#5813](https://github.com/wled/WLED/issues/5813) | -- | WebUI effects list incomplete/broken with chunked responses | **Closed** -- root cause explained via #5808 follow-up comment |

## Internal / fork-only issues

Issues tracked in Forgejo only -- not upstream PR candidates.

| Issue | Title | Status |
|-------|-------|--------|
| [#1](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/1) | feat(ddp): per-segment targeting with dual-mode routing + FPS=0 lockup fix | Implemented -- see pr/ddp-per-segment |
| [#2](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/2) | DDP high-rate crash at 670+ FPS (P5) | Mitigated by pr/ddp-flood-hardening rate limiter + heap guard |
| [#3](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/3) | /reset endpoint does not reboot device (P4) | Open -- PPP serial interaction with esp_restart() under investigation |
| [#4](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/4) | Web UI for per-segment DDP eligibility mask | Open -- API-only via /json/cfg; UI work deferred |
| [#28](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/28) | ESPAsyncWebServer: RESPONSE_WAIT_ACK exits before ACKs on chunked path | Tracking -- upstream discussion filed at ESP32Async/ESPAsyncWebServer#472 |

## Recommended submit order

1. **pr/segment-eligibility** -- independent, no deps, upstream interest via #5810 comments
2. pr/mdns-ppp-crash-fix -- companion to ppp-transport
3. pr/bus-skip-show -- independent, no deps
4. pr/ddp-rle-codec -- codec only
5. pr/ddp-compressed-receiver -- depends on #4
6. pr/ddp-compressed -- full bundle, after #4 and #5
7. pr/text-aa-fonts -- no deps
8. pr/argb-passthrough -- note wled_ppp.cpp hunk is fork-specific
9. pr/tft-bus-matrix -- M5StickC-specific, factor out AXP192; depends on #3
10. pr/ppp-transport -- with pr/mdns-ppp-crash-fix companion
11. pr/ddp-per-segment -- Mode A destination routing, after segment-eligibility lands
12. pr/slip-transport -- low priority
13. pr/effects-deferred-fade -- after V3 resolves text/gradient interaction
14. pr/text-drop-shadow -- after pr/text-aa-fonts (stub, pending implementation)
15. pr/arduino-esp32-mdns-guard -- to espressif/arduino-esp32
16. pr/arduino-esp32-netif-lazy-init -- to espressif/arduino-esp32
17. pr/esp-idf-lcp-echo-docs -- to espressif/esp-idf
