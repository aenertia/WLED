# PR Tracking — WLED Upstream Contributions

**Fork**: [aenertia/WLED](https://github.com/aenertia/WLED)
**GitHub fork**: https://github.com/aenertia/WLED
**Branch base**: All pr/* branches are on `upstream/main` (9ebdbdea)
**Upstream**: [Aircoookie/WLED](https://github.com/Aircoookie/WLED)
**Active branch**: `dev/ppp-wifi`
**Last updated**: Session 21 (August 2026)

## Status summary

| Metric | Value |
|--------|-------|
| PR branches | 23 |
| Forgejo issues | #1–#27 |
| Device soak test | 250min+ continuous, reset=1 (POWERON) |
| Upstream submissions | 5 PRs open + 2 issues — [#5804](https://github.com/wled/WLED/pull/5804), [#5805](https://github.com/wled/WLED/pull/5805), [#5806](https://github.com/wled/WLED/pull/5806), [#5807](https://github.com/wled/WLED/pull/5807), [#5808](https://github.com/wled/WLED/pull/5808); issues [#5810](https://github.com/wled/WLED/issues/5810), [#5811](https://github.com/wled/WLED/issues/5811) |

## Phase 1 — Bug fixes (submit first)

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/mdns-ppp-crash-fix` | [#18](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/18) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/mdns-ppp-crash-fix/DRAFT_PR.md) | mDNS NULL netif crash on WiFi STA disconnect under PPP | Ready |
| `pr/chunked-json-fix` | [#5](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/5) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/chunked-json-fix/DRAFT_PR.md) | Content-Length for /json/fxdata — prevents truncation on slow links | **Submitted** — [wled/WLED#5808](https://github.com/wled/WLED/pull/5808) · CR feedback addressed |
| `pr/audioreactive-pdm-fix` | [#17](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/17) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/audioreactive-pdm-fix/DRAFT_PR.md) | Skip i2s_set_clk() for PDM mode on IDF 5.x | **Submitted** — [wled/WLED#5807](https://github.com/wled/WLED/pull/5807) |

## Phase 2 — Small upstream fixes (high viability)

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/ws-state-only-broadcast` | [#23](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/23) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ws-state-only-broadcast/DRAFT_PR.md) | Skip serializeInfo() on WebSocket broadcast — saves 4–6KB heap | **Submitted** — [wled/WLED#5806](https://github.com/wled/WLED/pull/5806) · CR feedback addressed |
| `pr/segment-name-race-fix` | [#24](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/24) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/segment-name-race-fix/DRAFT_PR.md) | alloc-fill-swap-free in Segment::setName() — dual-core race fix | **Submitted** — [wled/WLED#5805](https://github.com/wled/WLED/pull/5805) · CR feedback addressed |
| `pr/watchdog-idf5-compat` | [#25](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/25) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/watchdog-idf5-compat/DRAFT_PR.md) | esp_task_wdt_config_t struct API for IDF 5.x | **Submitted** — [wled/WLED#5804](https://github.com/wled/WLED/pull/5804) |

## Phase 3 — DDP compression

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/ddp-rle-codec` | [#6](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/6) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-rle-codec/DRAFT_PR.md) | Header-only RLE codec (ddp_compress.h) | Ready |
| `pr/ddp-compressed-receiver` | [#7](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/7) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-compressed-receiver/DRAFT_PR.md) | Compressed DDP decode in handleDDPPacket() — depends on ddp-rle-codec | Ready |
| `pr/ddp-compressed` | [#16](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/16) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-compressed/DRAFT_PR.md) | Full stack: codec + receiver + tools + spec | Ready |
| *(upstream issue)* | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/dev/ppp-wifi-internal/DDP-COMPRESSION-ISSUE.md) | DDP compression extension — gauging upstream interest | **Issue open** — [wled/WLED#5810](https://github.com/wled/WLED/issues/5810) |

## Phase 4 — Effects

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/effects-fade-snap` | [#9](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/9) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/effects-fade-snap/DRAFT_PR.md) | Snap-to-target in fade_out/fadeToBlackBy | **Closed** — [wled/WLED#5809](https://github.com/wled/WLED/pull/5809) · maintainer rejected (changes visual behaviour); kept fork-local |
| `pr/effects-deferred-fade` | [#10](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/10) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/effects-deferred-fade/DRAFT_PR.md) | Deferred fade accumulator | **INCOMPLETE** — V2 broke scrolling text |

## Phase 5 — Text

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/text-aa-fonts` | [#11](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/11) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/text-aa-fonts/DRAFT_PR.md) | DejaVu Bold 18px + 40px 4bpp anti-aliased fonts | Ready |
| `pr/text-drop-shadow` | [#12](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/12) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/text-drop-shadow/DRAFT_PR.md) | Drop shadow with angle/distance/intensity | Stub — implementation pending |

## Phase 6 — Hardware / transport

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/argb-passthrough` | [#13](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/13) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/argb-passthrough/DRAFT_PR.md) | ARGB motherboard header passthrough via RMT | Ready |
| `pr/tft-bus-matrix` | [#14](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/14) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/tft-bus-matrix/DRAFT_PR.md) | BusSPIMatrix — SPI display as pixel matrix output bus | Ready · depends on `pr/bus-skip-show` |
| `pr/ppp-transport` | [#15](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/15) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ppp-transport/DRAFT_PR.md) | PPP-over-serial network transport (WLED_USE_PPP) | Ready · **Issue open** — [wled/WLED#5811](https://github.com/wled/WLED/issues/5811) |
| `pr/slip-transport` | [#8](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/8) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/slip-transport/DRAFT_PR.md) | SLIP transport — low priority | Ready · referenced in [#5811](https://github.com/wled/WLED/issues/5811) |

## Phase 7 — Performance

| Branch | Forgejo | GitHub | Description | Status |
|--------|---------|--------|-------------|--------|
| `pr/bus-skip-show` | [#22](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/22) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/bus-skip-show/DRAFT_PR.md) | Bus idle-skip gate via `hasIdleSkip()` virtual — Hub75, Network, SPI matrix opt in | Ready · independent, no deps |
| `pr/ddp-per-segment` | [#26](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/26) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-per-segment/DRAFT_PR.md) | Dual-mode DDP routing (Mode A destination byte, Mode B eligibility mask) | Ready |

## Phase 8 — Upstream component fixes (target: arduino-esp32 / esp-idf)

| Branch | Forgejo | GitHub | Target repo | Description | Status |
|--------|---------|--------|-------------|-------------|--------|
| `pr/arduino-esp32-mdns-guard` | [#19](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/19) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/arduino-esp32-mdns-guard/DRAFT_PR.md) | espressif/arduino-esp32 | ESPmDNS::end() NULL deref guard | Ready |
| `pr/arduino-esp32-netif-lazy-init` | [#20](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/20) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/arduino-esp32-netif-lazy-init/DRAFT_PR.md) | espressif/arduino-esp32 | esp_netif_init() before netif creation | Ready |
| `pr/esp-idf-lcp-echo-docs` | [#21](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/21) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/esp-idf-lcp-echo-docs/DRAFT_PR.md) | espressif/esp-idf | LWIP_ENABLE_LCP_ECHO Kconfig warning | Ready |

## Deferred

| Branch | Forgejo | GitHub | Description | Reason |
|--------|---------|--------|-------------|--------|
| `pr/ddp-flood-hardening` | [#27](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/27) | [DRAFT_PR.md](https://github.com/aenertia/WLED/blob/pr/ddp-flood-hardening/DRAFT_PR.md) | Heap guard + rate limiter + starvation detector | Interleaved with ddp-per-segment in e131.cpp; defer until ddp-per-segment lands |

## Internal / Fork-only issues

Issues tracked in Forgejo only — not upstream PR candidates. Fork-specific bugs, features, and UI work.

| Issue | Title | Status |
|-------|-------|--------|
| [#1](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/1) | feat(ddp): per-segment targeting with dual-mode routing + FPS=0 lockup fix | Implemented — see `pr/ddp-per-segment` |
| [#2](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/2) | DDP high-rate crash at 670+ FPS (P5) | Mitigated by `pr/ddp-flood-hardening` rate limiter + heap guard |
| [#3](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/3) | /reset endpoint doesn't reboot device (P4) | Open — PPP serial interaction with esp_restart() under investigation |
| [#4](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/4) | Web UI for per-segment DDP eligibility mask | Open — API-only via /json/cfg; UI work deferred |

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
