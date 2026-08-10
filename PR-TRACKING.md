# PR Tracking — WLED Upstream Contributions

**Fork**: [aenertia/wled](https://git.awa.3d.ae.net.nz/aenertia/wled)
**Upstream**: [wled/WLED](https://github.com/wled/WLED)
**Base**: upstream/main (`d9b9a846`)

## Branches

### Full Fork

| Branch | Description | Status |
|--------|-------------|--------|
| `noWLED-ppp` | Full no-WLED fork (WiFi deliberately dropped, all features) | Active — 91 commits on upstream/main |
| `main` | Original pre-rebase branch | Frozen |

### PR Topic Branches (each rebased on upstream/main HEAD)

#### Phase 1 — Quick Wins (build contributor credibility)

| Branch | Files | Lines | Upstream Issue | Status | Review |
|--------|-------|-------|----------------|--------|--------|
| [`pr/effects-fade-snap`](../compare/main...pr/effects-fade-snap) | 2 | +68 | [#4976](https://github.com/wled/WLED/issues/4976) (open, `keep`) | Ready | Snap-to-target fix for interrupted transitions |
| [`pr/text-drop-shadow`](../compare/main...pr/text-drop-shadow) | 3 | +71 | None — create issue first | Ready | Drop shadow with angle/distance/intensity |

#### Phase 2 — Independent Features (submit in parallel)

| Branch | Files | Lines | Upstream Issue | Status | Review |
|--------|-------|-------|----------------|--------|--------|
| [`pr/ddp-rle-codec`](../compare/main...pr/ddp-rle-codec) | 2 | +182 | None — create issue first | Ready | Standalone delta+RLE compression codec |
| [`pr/argb-passthrough`](../compare/main...pr/argb-passthrough) | 3 | +193 | [#2675](https://github.com/wled/WLED/issues/2675), [#1116](https://github.com/wled/WLED/issues/1116) (adjacent) | Ready | RMT RX→TX motherboard ARGB signal relay |
| [`pr/tft-bus-matrix`](../compare/main...pr/tft-bus-matrix) | 5 | +200 | None — create issue first | Ready | TFT display as WLED pixel matrix output bus |

#### Phase 3 — DDP Receiver (depends on Phase 2)

| Branch | Files | Lines | Upstream Issue | Depends On | Review |
|--------|-------|-------|----------------|------------|--------|
| [`pr/ddp-compressed-receiver`](../compare/main...pr/ddp-compressed-receiver) | 4 | +266 | Same as ddp-rle-codec | `pr/ddp-rle-codec` merged | Compressed DDP decode in handleDDPPacket() |

#### Phase 4 — PPP Transport (flagship)

| Branch | Files | Lines | Upstream Issue | Status | Review |
|--------|-------|-------|----------------|--------|--------|
| [`pr/ppp-transport`](../compare/main...pr/ppp-transport) | 9 | +408 | None — create Discussion first | Ready | PPP-over-serial: full WLED over USB/UART |

#### Phase 5 — Follow-ups

| Branch | Files | Lines | Upstream Issue | Depends On | Review |
|--------|-------|-------|----------------|------------|--------|
| [`pr/effects-deferred-fade`](../compare/main...pr/effects-deferred-fade) | 3 | +82 | Related to [#4976](https://github.com/wled/WLED/issues/4976) | `pr/effects-fade-snap` merged | Compositor-level deferred fade accumulator |
| [`pr/text-aa-fonts`](../compare/main...pr/text-aa-fonts) | 3 | +3352 | None — create issue first | None | 4bpp anti-aliased DejaVu Bold 18px + 40px |
| [`pr/slip-transport`](../compare/main...pr/slip-transport) | 3 | +184 | None | None | SLIP serial transport + compressed DDP bridge |

## Dependency Graph

```
Phase 1:  pr/effects-fade-snap ──────────────► pr/effects-deferred-fade (Phase 5)
          pr/text-drop-shadow ───────────────► pr/text-aa-fonts (Phase 5)

Phase 2:  pr/ddp-rle-codec ──────────────────► pr/ddp-compressed-receiver (Phase 3)
          pr/argb-passthrough                  (independent)
          pr/tft-bus-matrix                    (independent)

Phase 4:  pr/ppp-transport                     (after Discussion + credibility)

Phase 5:  pr/slip-transport + DDP compression  → libwled-serial (app driver)
```

## Upstream Issues to Create

Before submitting PRs to upstream, create these feature request issues on github.com/wled/WLED:

1. **PPP Transport** — Post as GitHub Discussion first, not issue. Reference #4990, #1382, #5697.
2. **DDP Compression** — Issue. Reference #5755 (WS fragmentation), #4320 (DDP bandwidth).
3. **TFT Matrix Bus** — Issue. Reference #2197 (Framebuffer::GFX proposal).
4. **Text AA Fonts** — Issue. Reference PR #5372 (custom fonts).
5. **Text Drop Shadow** — Issue. Can bundle with AA fonts issue.

Issues NOT needed (existing match):
- `pr/effects-fade-snap` → links to [#4976](https://github.com/wled/WLED/issues/4976) directly
- `pr/argb-passthrough` → comment on [#2675](https://github.com/wled/WLED/issues/2675) / [#1116](https://github.com/wled/WLED/issues/1116)

## Key Upstream PRs to Coordinate With

| PR | Title | Status | Impact |
|----|-------|--------|--------|
| [#5650](https://github.com/wled/WLED/pull/5650) | Independent Ethernet/WiFi IP + primary netif selection | Open (v17.0) | PPP transport should align with this netif pattern |
| [#5697](https://github.com/wled/WLED/pull/5697) | ESP32-P4 ethernet-only build | Closed | Template for WiFi-less build mode |
| [#5774](https://github.com/wled/WLED/pull/5774) | Split udp.cpp into per-protocol files | Open | DDP receiver changes coordinate with this refactor |

## SLIP + DDP Compression Strategy (libwled-serial)

SLIP transport + transparent DDP compression = USB pixel streaming driver for app integration.

```
OpenRGB/CoolerCtrl ──DDP──► libwled-serial ──compress──► SLIP serial ──► ESP32 ──► LEDs
```

Ship as embeddable library for CoolerControl, LiquidCtl, OpenRGB. No IP stack needed on host.
Host-side bridge handles compression; ESP32 side is SLIP + compressed DDP receiver.

## Each Branch Has

- `PR_REFERENCE.md` — upstream issues, adjacent discussions, suggested issue templates
- Feature code rebased on upstream/main HEAD
- Integration glue files where needed (Network.cpp, wled.cpp, bus_manager.cpp)

## Review Commands

```bash
# Compare any branch to upstream
git diff upstream/main pr/ppp-transport

# Read the reference doc
git show pr/ppp-transport:PR_REFERENCE.md

# Check what files changed
git diff --stat upstream/main pr/effects-fade-snap

# See commit messages
git log --oneline pr/ddp-rle-codec --not upstream/main
```
