---
slug: next-phase-remediations
status: awaiting-approval
intent: clear
review_required: false
pending-action: write .omo/plans/next-phase-remediations.md
approach: 3 waves — review remediations (4 parallel fixes), Pico DDP mangle (4 sequential steps), compile gates (ESP32 + Pico)
---

# Draft: next-phase-remediations

## Components (topology ledger)

| id | outcome | status | evidence |
|---|---|---|---|
| W1-remediations | Fix 4 review findings (ddp_push guard, 2D deferred fade, transform clamp, board cache) | active | compile gate |
| W2-pico-mangle | Pico DDP NAT+mangle compressor (~120 lines in main.c + shared header) | active | Pico compile gate |
| W3-compile | All 11 ESP32 targets + Pico .uf2 compile clean | active | compile output |

## Open assumptions (announced defaults)

| assumption | adopted default | rationale | reversible? |
|---|---|---|---|
| Pico DDP interception method | LWIP_HOOK_IP4_INPUT | Cleanest hook for forwarded packets; udp_recv only catches packets destined for Pico itself | yes |
| Pico prev-frame buffer | Static 1440 bytes (DDP_CHANNELS_PER_PACKET) | Handles one DDP packet at a time; multi-packet frames use offset-keyed slots | yes |
| Keyframe interval | Hardcoded 30 frames | Matches ADR spec; no config needed | yes |
| Transform loop clamp strategy | Use numExplicit as pixel count hint when > 0 | Prevents CPU amplification while preserving full-strip transforms | yes |
| SSH to koero | `distrobox-host-exec ssh -o GSSAPIAuthentication=no koero.3d.ae.net.nz "bash --noprofile --norc -c '...'"` | Avoids GSSAPI timeout + NFS-stalling login profile | no |

## Findings (cited - path:lines)

- ddp_push label: e131.cpp:160, referenced only from :107 inside #ifdef guard → unused-label warning on non-compressed builds
- getPixelColorXYRaw: FX.h:555, bypasses deferred fade (returns pixels[XY] directly)
- Transform loop: e131.cpp:120-131, iterates start..totalLen regardless of packet scope
- m5stick-c board: PlatformIO Tasmota platform missing m5stick_c variant dir in buildcache
- Pico forwarding: main.c uses IP_FORWARD=1 (L3 bridge), DDP interception needs application-layer hook

## Decisions (with rationale)

1. LWIP_HOOK_IP4_INPUT over process_usb_rx inspection — the hook fires at the IP layer for all forwarded packets, cleaner than parsing raw Ethernet frames
2. Static buffers over dynamic allocation — Pico has ~120KB free, 6KB static is trivial and avoids pbuf pool pressure
3. Adaptive RLE (rle_encode_adaptive) from shared ddp_compress.h — same algorithm on both Pico and ESP32, single source of truth

## Scope IN

- 4 review remediations (ddp_push, 2D deferred, transform clamp, board fix)
- Pico DDP mangle compressor (interception hook + compression + keyframes)
- Shared ddp_compress.h for Pico (copy + adapt)
- Compile gates for both ESP32 (11 targets) and Pico (.uf2)

## Scope OUT (Must NOT have)

- No BusNetwork sender compression
- No OpenRGB/HA client patches
- No hardware flashing or physical testing
- No web UI changes
- No FX.cpp modifications

## Open questions

None — all design decisions resolved from prior analysis.

## Approval gate
status: awaiting-approval
