# PR Tracking — WLED Upstream Contributions

**Fork**: [aenertia/WLED](https://github.com/aenertia/WLED)
**GitHub fork**: https://github.com/aenertia/WLED
**Branch base**: All pr/* branches are on `upstream/main` (9ebdbdea)
**Upstream**: [Aircoookie/WLED](https://github.com/Aircoookie/WLED)
**Active branch**: `dev/ppp-wifi`
**Last updated**: Session 18 (August 2026)

## Status summary

| Metric | Value |
|--------|-------|
| PR branches | 21 |
| Forgejo issues | #5–#22+ |
| Device soak test | 250min+ continuous, reset=1 (POWERON) |
| Upstream submissions | None yet — all Ready or INCOMPLETE |

## Phase 1 — Bug fixes (submit first)

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/mdns-ppp-crash-fix` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/mdns-ppp-crash-fix/DRAFT_PR.md) | mDNS NULL netif crash on WiFi STA disconnect under PPP | Ready |
| `pr/chunked-json-fix` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/chunked-json-fix/DRAFT_PR.md) | Content-Length for /json/fxdata — prevents truncation on slow links | Ready |
| `pr/audioreactive-pdm-fix` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/audioreactive-pdm-fix/DRAFT_PR.md) | Skip i2s_set_clk() for PDM mode on IDF 5.x | Ready |

## Phase 2 — Small upstream fixes (high viability)

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/ws-state-only-broadcast` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ws-state-only-broadcast/DRAFT_PR.md) | Skip serializeInfo() on WebSocket broadcast — saves 4–6KB heap | Ready |
| `pr/segment-name-race-fix` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/segment-name-race-fix/DRAFT_PR.md) | alloc-fill-swap-free in Segment::setName() — dual-core race fix | Ready |
| `pr/watchdog-idf5-compat` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/watchdog-idf5-compat/DRAFT_PR.md) | esp_task_wdt_config_t struct API for IDF 5.x | Ready |

## Phase 3 — DDP compression

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/ddp-rle-codec` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-rle-codec/DRAFT_PR.md) | Header-only RLE codec (ddp_compress.h) | Ready |
| `pr/ddp-compressed-receiver` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-compressed-receiver/DRAFT_PR.md) | Compressed DDP decode in handleDDPPacket() — depends on ddp-rle-codec | Ready |
| `pr/ddp-compressed` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-compressed/DRAFT_PR.md) | Full stack: codec + receiver + tools + spec | Ready |

## Phase 4 — Effects

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/effects-fade-snap` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/effects-fade-snap/DRAFT_PR.md) | Snap-to-target in fade_out/fadeToBlackBy | Ready |
| `pr/effects-deferred-fade` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/effects-deferred-fade/DRAFT_PR.md) | Deferred fade accumulator | **INCOMPLETE** — V2 broke scrolling text |

## Phase 5 — Text

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/text-aa-fonts` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/text-aa-fonts/DRAFT_PR.md) | DejaVu Bold 18px + 40px 4bpp anti-aliased fonts | Ready |
| `pr/text-drop-shadow` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/text-drop-shadow/DRAFT_PR.md) | Drop shadow with angle/distance/intensity | Stub — implementation pending |

## Phase 6 — Hardware / transport

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/argb-passthrough` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/argb-passthrough/DRAFT_PR.md) | ARGB motherboard header passthrough via RMT | Ready |
| `pr/tft-bus-matrix` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/tft-bus-matrix/DRAFT_PR.md) | TFT display as WLED pixel matrix output bus | Ready |
| `pr/ppp-transport` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ppp-transport/DRAFT_PR.md) | PPP-over-serial network transport (WLED_USE_PPP) | Ready |
| `pr/slip-transport` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/slip-transport/DRAFT_PR.md) | SLIP transport — low priority | Ready |

## Phase 7 — Performance

| Branch | GitHub | Description | Status |
|--------|--------|-------------|--------|
| `pr/ddp-per-segment` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-per-segment/DRAFT_PR.md) | Dual-mode DDP routing (Mode A destination byte, Mode B eligibility mask) | Ready |

## Phase 8 — Upstream component fixes (target: arduino-esp32 / esp-idf)

| Branch | GitHub | Target repo | Description | Status |
|--------|--------|-------------|-------------|--------|
| `pr/arduino-esp32-mdns-guard` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/arduino-esp32-mdns-guard/DRAFT_PR.md) | espressif/arduino-esp32 | ESPmDNS::end() NULL deref guard | Ready |
| `pr/arduino-esp32-netif-lazy-init` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/arduino-esp32-netif-lazy-init/DRAFT_PR.md) | espressif/arduino-esp32 | esp_netif_init() before netif creation | Ready |
| `pr/esp-idf-lcp-echo-docs` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/esp-idf-lcp-echo-docs/DRAFT_PR.md) | espressif/esp-idf | LWIP_ENABLE_LCP_ECHO Kconfig warning | Ready |

## Deferred

| Branch | GitHub | Description | Reason |
|--------|--------|-------------|--------|
| `pr/bus-skip-show` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/bus-skip-show/DRAFT_PR.md) | Skip show() for idle buses + showFrozenSegs() DDP fast path | Depends on pr/ddp-per-segment landing first; code interleaved at function level |
| `pr/ddp-flood-hardening` | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-flood-hardening/DRAFT_PR.md) | Heap guard + rate limiter + starvation detector | Interleaved with ddp-per-segment in e131.cpp; defer until ddp-per-segment lands |

## Recommended submit order

1. `pr/mdns-ppp-crash-fix` — companion to ppp-transport, submit first
2. `pr/chunked-json-fix` — minimal, no deps
3. `pr/audioreactive-pdm-fix` — to AudioReactive usermod
4. `pr/ws-state-only-broadcast` — 3 lines, no deps
5. `pr/segment-name-race-fix` — 15 lines, no deps
6. `pr/watchdog-idf5-compat` — 15 lines, no deps
7. `pr/ddp-rle-codec` — codec only
8. `pr/ddp-compressed-receiver` — depends on #7
9. `pr/effects-fade-snap` — 10 lines, no deps
10. `pr/text-aa-fonts` — no deps
11. `pr/text-drop-shadow` — after #10 (stub, pending implementation)
12. `pr/argb-passthrough` — note wled_ppp.cpp hunk is fork-specific
13. `pr/tft-bus-matrix` — M5StickC-specific, factor out AXP192
14. `pr/ppp-transport` — with pr/mdns-ppp-crash-fix companion
15. `pr/ddp-compressed` — full bundle, after #7 and #8
16. `pr/ddp-per-segment` — after DDP compression lands
17. `pr/slip-transport` — low priority
18. `pr/effects-deferred-fade` — after V3 resolves text/gradient interaction
19. `pr/arduino-esp32-mdns-guard` — to espressif/arduino-esp32
20. `pr/arduino-esp32-netif-lazy-init` — to espressif/arduino-esp32
21. `pr/esp-idf-lcp-echo-docs` — to espressif/esp-idf
