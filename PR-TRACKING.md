# PR Tracking -- WLED Upstream Contributions

**Fork**: [aenertia/WLED](https://github.com/aenertia/WLED)
**GitHub fork**: https://github.com/aenertia/WLED
**Forgejo**: https://git.awa.3d.ae.net.nz/aenertia/wled
**Branch base**: All pr/* branches are on `upstream/main` (9ebdbdea)
**Upstream**: [Aircoookie/WLED](https://github.com/Aircoookie/WLED)
**Active branch**: `dev/ppp-wifi`
**Last updated**: Session 28 (August 2026)

## Status summary

| Metric | Value |
|--------|-------|
| PR branches | 20 local + upstream-pr/segment-eligibility |
| Forgejo issues | #1--#28 |
| Upstream PRs | 3 open, 1 merged, 2 closed |
| Upstream issues | 2 open, 1 closed |
| ESPAsync upstream issues | 1 filed -- [ESP32Async/ESPAsyncWebServer discussion #472](https://github.com/ESP32Async/ESPAsyncWebServer/discussions/472) (see Forgejo [#28](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/28)) |
| Device soak test | 250min+ continuous, reset=1 (POWERON) |

## Active upstream PRs

| PR | Branch | Forgejo | GitHub | Description | Status |
|----|--------|---------|--------|-------------|--------|
| [#5804](https://github.com/wled/WLED/pull/5804) | `pr/watchdog-idf5-compat` | [#25](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/25) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/watchdog-idf5-compat/DRAFT_PR.md) | esp_task_wdt_config_t struct API for IDF 5.x | Open |
| [#5806](https://github.com/wled/WLED/pull/5806) | `pr/ws-state-only-broadcast` | [#23](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/23) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ws-state-only-broadcast/DRAFT_PR.md) | Skip serializeInfo() on WS broadcast -- saves 4-6KB heap | Open -- CR feedback addressed |
| [#5807](https://github.com/wled/WLED/pull/5807) | `pr/audioreactive-pdm-fix` | [#17](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/17) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/audioreactive-pdm-fix/DRAFT_PR.md) | Skip i2s_set_clk() for PDM mode on IDF 5.x | Open |

## Active upstream issues

| Issue | Description | Status |
|-------|-------------|--------|
| [#5810](https://github.com/wled/WLED/issues/5810) | DDP compression extension -- gauging interest | Open -- spec posted, feedback received |
| [#5811](https://github.com/wled/WLED/issues/5811) | PPP network transport for ESP32 | Open |

## Ready to submit

### Per-segment realtime eligibility (next up)

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/segment-eligibility` | [#26](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/26) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/segment-eligibility/DRAFT_PR.md) | Per-segment realtime eligibility mask (Mode B only, replaces useMainSegmentOnly) | Ready -- pushed to GitHub, merged into dev/ppp-wifi, validated on hardware |
| `upstream-pr/segment-eligibility` | -- | -- | Submission branch (pr/segment-eligibility sans DRAFT_PR.md) | Ready for manual PR submission |

Supersedes `pr/ddp-per-segment` for upstream. Mode A (destination-byte routing) excluded per softhack007 feedback ("separate PR"). `pr/ddp-per-segment` retained fork-local for Mode A + Mode B combined.

### Bug fixes

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/mdns-ppp-crash-fix` | [#18](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/18) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/mdns-ppp-crash-fix/DRAFT_PR.md) | mDNS NULL netif crash on WiFi STA disconnect under PPP | Ready |

### DDP compression

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/ddp-rle-codec` | [#6](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/6) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-rle-codec/DRAFT_PR.md) | Header-only RLE codec (ddp_compress.h) | Ready |
| `pr/ddp-compressed-receiver` | [#7](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/7) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-compressed-receiver/DRAFT_PR.md) | Compressed DDP decode in handleDDPPacket() -- depends on ddp-rle-codec | Ready |
| `pr/ddp-compressed` | [#16](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/16) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-compressed/DRAFT_PR.md) | Full stack: codec + receiver + tools + spec | Ready |

### Effects

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/effects-deferred-fade` | [#10](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/10) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/effects-deferred-fade/DRAFT_PR.md) | Deferred fade accumulator | INCOMPLETE -- V2 broke scrolling text |

### Text

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/text-aa-fonts` | [#11](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/11) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/text-aa-fonts/DRAFT_PR.md) | DejaVu Bold 18px + 40px 4bpp anti-aliased fonts | Ready |
| `pr/text-drop-shadow` | [#12](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/12) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/text-drop-shadow/DRAFT_PR.md) | Drop shadow with angle/distance/intensity | Stub -- implementation pending |

### Hardware / transport

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/argb-passthrough` | [#13](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/13) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/argb-passthrough/DRAFT_PR.md) | ARGB motherboard header passthrough via RMT | Ready |
| `pr/tft-bus-matrix` | [#14](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/14) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/tft-bus-matrix/DRAFT_PR.md) | BusSPIMatrix -- SPI display as pixel matrix output bus | Ready -- depends on pr/bus-skip-show |
| `pr/ppp-transport` | [#15](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/15) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ppp-transport/DRAFT_PR.md) | PPP-over-serial network transport (WLED_USE_PPP) | Ready -- issue [#5811](https://github.com/wled/WLED/issues/5811) open |
| `pr/slip-transport` | [#8](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/8) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/slip-transport/DRAFT_PR.md) | SLIP transport -- low priority -- referenced in [#5811](https://github.com/wled/WLED/issues/5811) |

### Performance

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/bus-skip-show` | [#22](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/22) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/bus-skip-show/DRAFT_PR.md) | Generalized idle-skip gate -- skip show() for buses with no active segments | Ready -- independent, no deps |
| `pr/ddp-per-segment` | [#26](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/26) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-per-segment/DRAFT_PR.md) | Dual-mode DDP routing (Mode A + Mode B combined) | Fork-local -- upstream submission via pr/segment-eligibility (Mode B only) |

### Upstream component fixes (target: arduino-esp32 / esp-idf / ESPAsyncWebServer)

| Branch | Forgejo | GitHub | Target repo | Description | Status |
|--------|---------|--------|-------------|-------------|--------|
| `pr/arduino-esp32-mdns-guard` | [#19](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/19) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/arduino-esp32-mdns-guard/DRAFT_PR.md) | espressif/arduino-esp32 | ESPmDNS::end() NULL deref guard | Ready |
| `pr/arduino-esp32-netif-lazy-init` | [#20](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/20) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/arduino-esp32-netif-lazy-init/DRAFT_PR.md) | espressif/arduino-esp32 | esp_netif_init() before netif creation | Ready |
| `pr/esp-idf-lcp-echo-docs` | [#21](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/21) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/esp-idf-lcp-echo-docs/DRAFT_PR.md) | espressif/esp-idf | LWIP_ENABLE_LCP_ECHO Kconfig warning | Ready |

### Deferred

| Branch | Forgejo | GitHub | Description | Reason |
|--------|---------|--------|-------------|--------|
| `pr/ddp-flood-hardening` | [#27](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/27) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-flood-hardening/DRAFT_PR.md) | Heap guard + rate limiter + starvation detector | Interleaved with ddp-per-segment in e131.cpp; defer until segment-eligibility lands upstream |

## Closed / merged upstream

| PR | Branch | Forgejo | GitHub | Description | Outcome |
|----|--------|---------|--------|-------------|---------|
| [#5805](https://github.com/wled/WLED/pull/5805) | `pr/segment-name-race-fix` | [#24](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/24) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/segment-name-race-fix/DRAFT_PR.md) | alloc-fill-swap-free in Segment::setName() -- dual-core race fix | **Merged** upstream -- comments stripped per reviewer feedback (190676d3) |
| [#5808](https://github.com/wled/WLED/pull/5808) | `pr/chunked-json-fix` | [#5](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/5) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/chunked-json-fix/DRAFT_PR.md) | Content-Length for /json/fxdata -- prevents truncation on slow links | **Closed** -- root cause found: sendChunked returned 0 instead of RESPONSE_TRY_AGAIN. Content-Length approach vetoed by willmmiles. Fix in json_chunked.h on dev/ppp-wifi |
| [#5809](https://github.com/wled/WLED/pull/5809) | `pr/effects-fade-snap` | [#9](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/9) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/effects-fade-snap/DRAFT_PR.md) | Snap-to-target in fade_out/fadeToBlackBy | **Closed** -- maintainer rejected (changes visual behaviour); kept fork-local |
| [#5813](https://github.com/wled/WLED/issues/5813) | -- | -- | -- | WebUI effects list incomplete/broken with chunked responses | **Closed** -- root cause explained via #5808 follow-up comment |

## Internal / fork-only issues

Issues tracked in Forgejo only -- not upstream PR candidates. Fork-specific bugs, features, and UI work.

| Issue | Title | Status |
|-------|-------|--------|
| [#1](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/1) | feat(ddp): per-segment targeting with dual-mode routing + FPS=0 lockup fix | Implemented -- see `pr/ddp-per-segment` |
| [#2](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/2) | DDP high-rate crash at 670+ FPS (P5) | Mitigated by `pr/ddp-flood-hardening` rate limiter + heap guard |
| [#3](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/3) | /reset endpoint does not reboot device (P4) | Open -- PPP serial interaction with esp_restart() under investigation |
| [#4](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/4) | Web UI for per-segment DDP eligibility mask | Open -- API-only via /json/cfg; UI work deferred |
| [#28](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/28) | ESPAsyncWebServer: AsyncAbstractResponse RESPONSE_WAIT_ACK exits before ACKs on chunked path | Tracking -- upstream issue to file at Aircoookie/ESPAsyncWebServer; discovered via wled/WLED#5808 |

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
