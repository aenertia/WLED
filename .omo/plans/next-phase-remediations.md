# next-phase-remediations - Work Plan

## TL;DR (For humans)

**What you'll get:** All code review findings fixed, m5stick-c board variant builds working, and the Pico bridge firmware upgraded to compress DDP packets in-flight (NAT+mangle) before they enter the PPP tunnel — completing the end-to-end compressed DDP path from PC to LEDs.

**Why this approach:** Three waves. Wave 1 fixes the review findings and board cache (quick, low-risk, unblocks compile gate). Wave 2 implements the Pico DDP mangle (the architectural centrepiece — compresses at the bottleneck). Wave 3 compiles everything and validates end-to-end.

**What it will NOT do:** No BusNetwork sender (WLED-to-WLED sync compression). No OpenRGB/HA patches (those apps send native compressed DDP when ready). No hardware flashing (separate task). No web UI changes.

**Effort:** Medium
**Risk:** Medium — Wave 1 is trivial; Wave 2 modifies the Pico's packet forwarding path (currently pure L3 bridge → application-aware DDP mangle)
**Decisions to sanity-check:** Pico DDP interception via LWIP_HOOK_IP4_INPUT vs per-packet inspection in process_usb_rx(). Pico prev-frame buffer sizing (3KB per 1000 LEDs).

Your next move: approve to begin. `$start-work next-phase-remediations`

---

> TL;DR (machine): Medium effort, medium risk. 3 waves: review remediations (4 fixes), Pico DDP mangle (~120 lines), compile-all gate. SSH to koero via `distrobox-host-exec ssh -o GSSAPIAuthentication=no koero.3d.ae.net.nz "bash --noprofile --norc -c '...'"`.

## Scope
### Must have

**Wave 1 — Review Remediations (4 items):**
1. Wrap `ddp_push:` label in `#ifdef WLED_ENABLE_DDP_COMPRESSION` to silence `-Wunused-label` (e131.cpp:160)
2. Route `getPixelColorXYRaw()` through deferred fade for 2D consistency (FX.h:555)
3. Clamp transform loop to `start..start+numPixels` instead of `start..totalLen` (e131.cpp:120-131)
4. Fix m5stick-c board variant on koero — reinstall Tasmota platform or symlink variant dir

**Wave 2 — Pico DDP Mangle (~120 lines):**
5. Add `ddp_compress.h` (copy from wled00/) to pico-bridge as shared header
6. Add DDP interception hook in Pico's IP forwarding path — intercept UDP/4048 packets destined for ESP32
7. Implement stateful DDP compressor: maintain prev-frame buffer, compute delta+RLE (or adaptive), rewrite DDP header flags, fix IP/UDP lengths and checksums
8. Keyframe logic: first frame uncompressed, every 30th frame uncompressed, brightness-change detection

**Wave 3 — Compile Gate:**
9. Compile all 11 WLED targets on koero (ESP32 side)
10. Compile Pico bridge firmware on koero via crossbuild-openaeos distrobox

### Must NOT have (guardrails, anti-slop, scope boundaries)
- No BusNetwork sender compression (WLED-to-WLED stays deferred)
- No OpenRGB or Home Assistant client patches
- No hardware flashing or physical testing (separate task)
- No web UI configuration changes
- No modifications to WLED effects in FX.cpp
- No changes to the PPP transport layer — Pico mangle rewrites DDP within existing IP/UDP/PPP path

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: none (no test framework) — compile-all + grep assertions + Pico compile
- ESP32 compile: `distrobox-host-exec ssh -o GSSAPIAuthentication=no koero.3d.ae.net.nz "bash --noprofile --norc -c '...'"`
- Pico compile: `distrobox-host-exec ssh -o GSSAPIAuthentication=no koero.3d.ae.net.nz "distrobox enter crossbuild-openaeos -- bash -c 'cd /var/mnt/koero/workspace/wled/pico-bridge && mkdir -p build && cd build && cmake .. && make 2>&1'"` (requires Pico SDK at `/var/mnt/koero/workspace/pico-sdk`)
- Evidence: .omo/evidence/

## Execution strategy
### Parallel execution waves

**Wave 1 (parallel):** T1-T4 — all independent, different files
**Wave 2 (sequential):** T5-T8 — Pico mangle, each step builds on previous
**Wave 3 (gate):** T9-T10 — compile both platforms

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| T1 (ddp_push ifdef) | — | T9 | T2, T3, T4 |
| T2 (2D deferred fade) | — | T9 | T1, T3, T4 |
| T3 (transform loop clamp) | — | T9 | T1, T2, T4 |
| T4 (m5stick-c board fix) | — | T9 | T1, T2, T3 |
| T5 (copy ddp_compress.h to Pico) | — | T6 | T1-T4 |
| T6 (DDP interception hook) | T5 | T7 | — |
| T7 (DDP compressor logic) | T6 | T8 | — |
| T8 (keyframe + adaptive) | T7 | T10 | — |
| T9 (ESP32 compile gate) | T1-T4 | F1-F4 | T10 |
| T10 (Pico compile gate) | T5-T8 | F1-F4 | T9 |

## Todos
> Implementation + Test = ONE todo. Never separate.

### Wave 1 — Review Remediations (parallel)

- [ ] 1. e131.cpp: wrap `ddp_push:` label in `#ifdef WLED_ENABLE_DDP_COMPRESSION`
  What to do: At e131.cpp:160, the `ddp_push:` label is unconditional but only referenced inside the `#ifdef WLED_ENABLE_DDP_COMPRESSION` block (line 107). Wrap the label: `#ifdef WLED_ENABLE_DDP_COMPRESSION` before `ddp_push:` and `#endif` after it but before `ddpSeenPush |= push;`. This silences the `-Wunused-label` warning on builds without compression.
  Must NOT do: Do not move or rename the label. Do not modify the push/sequence logic below it.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T9
  References: e131.cpp:107 (goto), e131.cpp:160 (label), compile warning in build log
  Acceptance criteria: `grep -A1 'ddp_push:' wled00/e131.cpp` shows the label inside `#ifdef` guard
  QA: compile with WLED_ENABLE_DDP_COMPRESSION undefined — no `-Wunused-label` warning
  Commit: N (batched into Wave 1)

- [ ] 2. FX.h: route `getPixelColorXYRaw()` through deferred fade
  What to do: At FX.h:555, `getPixelColorXYRaw()` returns `pixels[XY(x,y)]` directly, bypassing the deferred fade in `getPixelColorRaw()`. Under `#ifdef WLED_ENABLE_DDP_COMPRESSION`, change it to compute the XY index then call `getPixelColorRaw(idx)` to apply deferred fade. Keep original in `#else`. The XY lambda produces a 1D index into pixels[], which is what getPixelColorRaw() accepts.
  Must NOT do: Do not modify `setPixelColorXYRaw()`. Do not change the XY index calculation.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T9
  References: FX.h:541-548 (getPixelColorRaw with deferred fade), FX.h:555 (getPixelColorXYRaw), ADR §5.2
  Acceptance criteria: `grep -A5 'getPixelColorXYRaw' wled00/FX.h` shows `#ifdef` guard with call to `getPixelColorRaw`
  Commit: N (batched)

- [ ] 3. e131.cpp: clamp transform loop to packet-implied pixel range
  What to do: At e131.cpp:120 and 126, the SCALE_TOWARD and SCALE_MULT loops iterate `for (px = start; px < totalLen)` — processing ALL strip pixels regardless of packet scope. Change both loops to iterate `for (px = start; px < start + numExplicit && px < totalLen)` when `numExplicit > 0`, or `for (px = start; px < totalLen)` when `numExplicit == 0` (full-strip transform). This prevents a small packet from forcing iteration over an arbitrarily large strip. When no explicit writes follow, the transform IS intended for the full strip.
  Must NOT do: Do not change the explicit pixel write loop (Step B). Do not add new variables outside the compressed block.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T9
  References: e131.cpp:104-143 (transform branch), review finding MAJOR-1, refs/references/transform-compression-type.h
  Acceptance criteria: grep confirms loop upper bound includes `numExplicit` constraint
  Commit: N (batched)

- [ ] 4. koero: fix m5stick-c board variant for PlatformIO builds
  What to do: SSH to koero, diagnose the missing `pins_arduino.h` for m5stick-c board variant. The Tasmota platform (2026.02.30) may not include the m5stick-c variant directory. Options: (A) `pio pkg update` to refresh the platform, (B) symlink from the official Arduino-ESP32 framework's variants dir, (C) create a custom board JSON that points to a working variant. Try option A first. Command: `distrobox-host-exec ssh -o GSSAPIAuthentication=no koero.3d.ae.net.nz "bash --noprofile --norc -c 'export PATH=~/.local/bin:\$PATH && export PLATFORMIO_CORE_DIR=/var/mnt/koero/workspace/buildcache/platformio && cd /var/mnt/koero/workspace/wled && pio pkg update -p espressif32 2>&1 && pio run -e m5stickc_pico 2>&1 | tail -5'"`. If that fails, check if `framework-arduinoespressif32/variants/m5stick_c/` exists and create it with a minimal `pins_arduino.h` from upstream Arduino-ESP32.
  Must NOT do: Do not modify platformio.ini or platformio_override.ini. This is an infrastructure fix on koero, not a code change.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T9
  References: HANDOFF.md build section, koero.md host doc, compile log showing `pins_arduino.h: No such file or directory`
  Acceptance criteria: `pio run -e m5stickc_pico -e m5stickc_ppp_tft -e m5stickc_pico_tft` all SUCCESS
  Commit: N (infra fix, no code commit)

### Wave 2 — Pico DDP Mangle (sequential)

- [ ] 5. pico-bridge: add shared compression header
  What to do: Copy `wled00/ddp_compress.h` to `pico-bridge/src/ddp_compress.h`. Remove the `#ifdef WLED_ENABLE_DDP_COMPRESSION` / `#endif` guards (Pico always has compression enabled). Add the DDP flag constants (`DDP_FLAGS_COMPRESSED`, `DDP_COMP_TYPE_*`) directly in the header since the Pico doesn't include ESPAsyncE131.h. Verify it compiles standalone with `arm-none-eabi-gcc -c -fsyntax-only`.
  Must NOT do: Do not modify the original `wled00/ddp_compress.h`. Do not change the RLE algorithm.
  Parallelization: Wave 2 | Blocked by: — | Blocks: T6
  References: wled00/ddp_compress.h (source), pico-bridge/src/ (destination), ESPAsyncE131.h:62-72 (constants to duplicate)
  Acceptance criteria: `pico-bridge/src/ddp_compress.h` exists, contains `rle_encode`, `rle_encode_adaptive`, and `DDP_FLAGS_COMPRESSED`
  Commit: N (batched into Wave 2)

- [ ] 6. pico-bridge: add DDP packet interception in IP forwarding path
  What to do: In `pico-bridge/src/main.c`, add a DDP interception function. The Pico uses lwIP with `IP_FORWARD=1`. To intercept forwarded UDP/4048 packets, register a `udp_recv` callback on a raw UDP PCB bound to port 4048 on the USB-facing netif. When a DDP packet arrives from the host (USB side) destined for the ESP32 (169.254.7.1), the callback fires BEFORE IP forwarding. The callback can then: (A) compress the DDP payload, (B) rewrite the packet, (C) inject the modified packet into the PPP netif, (D) drop the original. Alternatively, inspect packets in `process_usb_rx()` before `ethernet_input()` — parse the Ethernet frame for UDP/4048 and divert. The UDP PCB approach is cleaner.
  CRITICAL: With `IP_FORWARD=1`, lwIP forwards packets NOT destined for the Pico. A UDP PCB bound on the Pico won't catch forwarded packets. Must use IP input hooks. The Pico is DUAL-STACK (IPv4 + IPv6) so DDP can arrive over either protocol. lwIP has separate hooks with different signatures:
  - `LWIP_HOOK_IP4_INPUT(struct pbuf *p, struct netif *inp, const ip4_addr_t *dest)` — 3 params
  - `LWIP_HOOK_IP6_INPUT(struct pbuf *p, struct netif *inp)` — 2 params
  Define BOTH in lwipopts.h, routing to a common handler:
  ```c
  #define LWIP_HOOK_IP4_INPUT(p,inp,dest) ddp_hook_ip_input(p, inp, 0)
  #define LWIP_HOOK_IP6_INPUT(p,inp)      ddp_hook_ip_input(p, inp, 1)
  ```
  The common `ddp_hook_ip_input(pbuf, netif, is_v6)` skips the IP header (20B for v4 / 40B for v6), checks for UDP protocol (v4: ip_hdr->proto==17, v6: next_header==17), checks dest port == 4048, then compresses and forwards. Returns 1 (consumed) for DDP, 0 (pass through) for everything else. Non-DDP traffic (HTTP, mDNS, etc.) is unaffected.
  Must NOT do: Do not break non-DDP traffic (HTTP, mDNS, etc. must still forward normally). Do not modify the PPP output path. Do not add blocking operations — the hook runs in the lwIP context.
  Parallelization: Wave 2 | Blocked by: T5 | Blocks: T7
  References: pico-bridge/src/main.c:578-708 (USB RX + IP forwarding path), pico-bridge/src/lwipopts.h:34 (IP_FORWARD), lwIP docs on LWIP_HOOK_IP4_INPUT, refs/compressed-ddp.md (protocol spec)
  Acceptance criteria: `grep LWIP_HOOK_IP4_INPUT pico-bridge/src/lwipopts.h` returns 1 match. `grep ddp_hook_ip4_input pico-bridge/src/main.c` returns 2+ matches (declaration + implementation).
  Commit: N (batched)

- [ ] 7. pico-bridge: implement DDP compressor in the interception hook
  What to do: In the `ddp_hook_ip4_input()` function from T6, implement the DDP compression logic:
  (A) Parse the incoming pbuf: extract IP header, UDP header, DDP header (flags, sequenceNum, dataType, channelOffset, dataLen, data).
  (B) Maintain a static prev-frame buffer: `static uint8_t ddp_prev_frame[DDP_MAX_FRAME_SIZE];` where `DDP_MAX_FRAME_SIZE = DDP_CHANNELS_PER_PACKET` (1440 bytes). Track offset to map multi-packet frames.
  (C) Call `rle_encode_adaptive(data, prev_frame + offset, dataLen, comp_buf, workspace, &compLen, &compType)` from ddp_compress.h.
  (D) If compression was beneficial (compLen < dataLen): allocate a new pbuf, copy IP+UDP+DDP headers, set `DDP_FLAGS_COMPRESSED` flag, set compression type in sequenceNum upper nibble, update dataLen to compLen, copy compressed payload. Fix UDP length and checksum. Forward via `udp_sendto_if()` to PPP netif.
  (E) If compression was not beneficial: pass the original packet through unmodified (return 0 to let IP_FORWARD handle it).
  (F) Update prev_frame buffer with current frame data (memcpy).
  Memory budget: prev_frame (1440B) + comp_buf (~1460B) + workspace (~2920B) = ~5.8KB. RP2040 has ~120KB free.
  Must NOT do: Do not modify the DDP data in-place (the original pbuf may be referenced elsewhere). Do not block — allocate pbufs from pool. Do not handle TCP or non-UDP traffic.
  Parallelization: Wave 2 | Blocked by: T6 | Blocks: T8
  References: pico-bridge/src/main.c, wled00/ddp_compress.h (rle_encode_adaptive), refs/compressed-ddp.md (wire format), ESPAsyncE131.h:51-72 (DDP constants and header structure)
  Acceptance criteria: `grep rle_encode_adaptive pico-bridge/src/main.c` returns 1+ match. `grep DDP_FLAGS_COMPRESSED pico-bridge/src/main.c` returns 1+ match. Static buffers total < 8KB.
  Commit: N (batched)

- [ ] 8. pico-bridge: keyframe and adaptive selection logic
  What to do: Add keyframe logic to the DDP compressor from T7:
  (A) Frame counter: `static uint16_t ddp_frame_count = 0;`. Increment per DDP PUSH packet.
  (B) First frame (`ddp_frame_count == 0`) or every 30th frame: send uncompressed (don't try compression, just pass through). This serves as a keyframe for delta sync recovery.
  (C) If `rle_encode_adaptive()` returns `DDP_COMP_TYPE_NONE` (raw is smaller), pass through uncompressed.
  (D) Track last-seen brightness via DDP header analysis (not directly available — skip brightness tracking for now, rely on keyframes for correction).
  (E) Reset frame counter on DDP sequence wrap (sequence number goes from 15 to 1).
  Must NOT do: Do not add configurable parameters — hardcode keyframe interval at 30.
  Parallelization: Wave 2 | Blocked by: T7 | Blocks: T10
  References: refs/compressed-ddp.md §Keyframe Strategy, wled00/ddp_compress.h (rle_encode_adaptive)
  Acceptance criteria: `grep ddp_frame_count pico-bridge/src/main.c` returns 1+ match. Keyframe logic sends uncompressed every 30 frames.
  Commit: Y | feat(pico): DDP NAT+mangle compressor with delta+RLE and keyframes

### Wave 3 — Compile Gates

- [ ] 9. ESP32 compile gate: all 11 custom targets on koero
  What to do: Push Wave 1 fixes, pull on koero, compile all 11 targets. Use `bash --noprofile --norc` to avoid NFS-hanging login profile. Must include the previously-failing m5stickc_pico, m5stickc_ppp_tft, m5stickc_pico_tft (after T4 board fix).
  Acceptance criteria: `pio run` exit 0 for all 11 environments. Zero `-Wunused-label` warnings from our code.
  Commit: Y | fix(ddp): review remediations — ddp_push guard, 2D deferred fade, transform loop clamp

- [ ] 10. Pico compile gate: build pico-bridge firmware on koero
  What to do: Push Wave 2 Pico changes, pull on koero, compile in crossbuild-openaeos distrobox. Command: `distrobox-host-exec ssh -o GSSAPIAuthentication=no koero.3d.ae.net.nz "distrobox enter crossbuild-openaeos -- bash -c 'export PICO_SDK_PATH=/var/mnt/koero/workspace/pico-sdk && cd /var/mnt/koero/workspace/wled/pico-bridge && rm -rf build && mkdir build && cd build && cmake .. -DPICO_BOARD=pico && make -j$(nproc) 2>&1'"`. Must produce a .uf2 file.
  Acceptance criteria: `pico-bridge/build/pico_ncm_ppp_bridge.uf2` exists and is > 200KB.
  Commit: Y | feat(pico): DDP NAT+mangle compressor with delta+RLE and keyframes

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE.
- [ ] F1. Plan compliance audit — every review finding addressed, every Wave 2 deliverable present
- [ ] F2. Code quality — grep for malloc/new in Pico hot path, verify pbuf lifecycle, check UDP checksum recalculation
- [ ] F3. End-to-end path audit — trace a DDP packet from PC through Pico compression through PPP through ESP32 handleDDPPacket decompression through setRealtimePixel. Every stage handles all 3 compression types.
- [ ] F4. Scope fidelity — no FX.cpp changes, no web UI, no BusNetwork sender, no hardware flashing

## Commit strategy
2 commits:
1. `fix(ddp): review remediations — ddp_push guard, 2D deferred fade, transform loop clamp` (after Wave 1 + T9)
2. `feat(pico): DDP NAT+mangle compressor with delta+RLE and keyframes` (after Wave 2 + T10)

## Success criteria
- All 11 ESP32 targets compile clean (including previously-failing m5stick-c variants)
- Pico bridge firmware compiles and produces .uf2
- Zero `-Wunused-label` warnings from our code
- `getPixelColorXYRaw()` applies deferred fade under compression flag
- Transform loop bounded by packet-implied pixel count
- Pico DDP mangle: intercepts UDP/4048, compresses with adaptive delta+RLE, keyframes every 30, passes non-DDP traffic unmodified
- Static memory on Pico for compression < 8KB
