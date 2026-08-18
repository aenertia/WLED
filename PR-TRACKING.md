# PR Tracking — WLED Upstream Contributions

**Fork**: [aenertia/wled](https://git.awa.3d.ae.net.nz/aenertia/wled)
**Upstream**: [Aircoookie/WLED](https://github.com/Aircoookie/WLED)
**Active branch**: `dev/ppp-wifi`
**Last updated**: Session 17 (August 2026)

## Status summary

| Metric | Value |
|--------|-------|
| PR branches | 18 |
| Forgejo issues | #5–#22 |
| Device soak test | 250min+ continuous, reset=1 (POWERON) |
| Upstream submissions | None yet — all Ready or INCOMPLETE |

## PR Topic Branches

All branches rebased on `forgejo/main`. Each has a DRAFT_PR.md with
code-grounded content, cherry-pick SHAs, GitHub issue cross-refs, and
upstream submission notes.

### Phase 1 — Bug fixes (submit first, build credibility)

| Branch | Forgejo | Cherry-pick | Description |
|--------|---------|-------------|-------------|
| `pr/mdns-ppp-crash-fix` | [#18](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/18) | IS the PR | mDNS NULL netif crash under PPP+WiFi+DDP flood |
| `pr/chunked-json-fix` | [#5](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/5) | IS the PR | Content-Length for /json/fxdata prevents truncation on slow links |
| `pr/audioreactive-pdm-fix` | [#17](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/17) | IS the PR | Skip i2s_set_clk() for PDM mode on IDF 5.x |

### Phase 2 — DDP compression (submit in order)

| Branch | Forgejo | Cherry-pick | Description |
|--------|---------|-------------|-------------|
| `pr/ddp-rle-codec` | [#6](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/6) | `a66f745e` | Header-only RLE codec (ddp_compress.h) |
| `pr/ddp-compressed-receiver` | [#7](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/7) | `c9d827fa` | Compressed DDP decode in handleDDPPacket() — depends on #6 |
| `pr/ddp-compressed` | [#16](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/16) | IS the PR | Full stack: codec + receiver + tools + spec |

### Phase 3 — Effects / performance

| Branch | Forgejo | Cherry-pick | Description |
|--------|---------|-------------|-------------|
| `pr/effects-fade-snap` | [#9](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/9) | `6e25fb7f` | Snap-to-target in fade_out/fadeToBlackBy — upstream issue #4976 |
| `pr/effects-deferred-fade` | [#10](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/10) | n/a | **INCOMPLETE** — V2 removed (broke scrolling text) |

### Phase 4 — Text effects

| Branch | Forgejo | Cherry-pick | Description |
|--------|---------|-------------|-------------|
| `pr/text-aa-fonts` | [#11](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/11) | `11d99490` | DejaVu Bold 18px + 40px fonts for scrolling text |
| `pr/text-drop-shadow` | [#12](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/12) | `304007a8`+`46096ac7` | Drop shadow with angle/distance/intensity |

### Phase 5 — Hardware / transport

| Branch | Forgejo | Cherry-pick | Upstream? | Description |
|--------|---------|-------------|-----------|-------------|
| `pr/argb-passthrough` | [#13](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/13) | `26e0cbac` | Yes (ESP32) | ARGB motherboard header passthrough via RMT |
| `pr/tft-bus-matrix` | [#14](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/14) | `42ef49e5` | M5StickC | TFT display as WLED pixel matrix output bus |
| `pr/ppp-transport` | [#15](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/15) | IS the PR | Yes | PPP-over-serial network transport (WLED_USE_PPP) |
| `pr/slip-transport` | [#8](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/8) | `99a565f0`+`d1a8d610` | Low priority | SLIP transport — ESP-IDF v5.1 removed from esp_netif |


### Phase 6 — Upstream component fixes (arduino-esp32 / esp-idf)

These branches target **third-party upstreams**, not Aircoookie/WLED.
Each DRAFT_PR.md contains the full diff and submission notes for the respective repo.

| Branch | Forgejo | Target repo | Description |
|--------|---------|-------------|-------------|
| `pr/arduino-esp32-mdns-guard` | [#19](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/19) | espressif/arduino-esp32 | ESPmDNS::end() NULL deref crash when begin() never called |
| `pr/arduino-esp32-netif-lazy-init` | [#20](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/20) | espressif/arduino-esp32 | PPP/ETH silent fail when esp_netif_init() not called before netif creation |
| `pr/esp-idf-lcp-echo-docs` | [#21](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/21) | espressif/esp-idf | LWIP_ENABLE_LCP_ECHO Kconfig help text — warns about link termination under load |


### Phase 7 — Bus skip-show + DDP realtime fast path

| Branch | Forgejo | Description |
|--------|---------|-------------|
| `pr/bus-skip-show` | [#22](https://git.awa.3d.ae.net.nz/aenertia/wled/issues/22) | Skip show() for idle slow buses (TFT/Hub75/Network) + showFrozenSegs() DDP fast path |

**Results**: DDP 45fps->119fps (+2.7x), IFS 52fps->86-93fps, TFT loopLag 23ms->0ms

## Zero-code-diff branches

These 9 branches have no code diff vs `forgejo/main` — feature code is
already in dev/ppp-wifi. When opening upstream GitHub PRs, use the
cherry-pick SHAs above to apply only the feature commits.

Branches with code diff vs forgejo/main (IS the PR):
- `pr/mdns-ppp-crash-fix` (53 lines)
- `pr/chunked-json-fix` (81 lines)
- `pr/audioreactive-pdm-fix` (30 lines)
- `pr/tft-bus-matrix` (127 lines)
- `pr/ppp-transport` (150 lines)
- `pr/ddp-compressed` (235 lines)

## Recommended submit order

1. `pr/mdns-ppp-crash-fix` — companion to ppp-transport, submit first
2. `pr/chunked-json-fix` — minimal, no deps
3. `pr/audioreactive-pdm-fix` — to AudioReactive usermod
4. `pr/ddp-rle-codec` — codec only
5. `pr/ddp-compressed-receiver` — depends on #4
6. `pr/effects-fade-snap` — 10 lines, no deps
7. `pr/text-aa-fonts` — no deps
8. `pr/text-drop-shadow` — after #7
9. `pr/argb-passthrough` — note wled_ppp.cpp hunk is fork-specific
10. `pr/ppp-transport` — with pr/mdns-ppp-crash-fix companion
11. `pr/tft-bus-matrix` — M5StickC-specific, factor out AXP192
12. `pr/ddp-compressed` — full bundle, after #4 and #5
13. `pr/effects-deferred-fade` — after V3 resolves text/gradient interaction
14. `pr/slip-transport` — low priority
