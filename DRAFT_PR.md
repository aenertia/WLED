# feat(ddp): header-only RLE codec for DDP pixel streaming

**Forgejo**: Fixes #6

## Summary

Adds `ddp_compress.h` — a self-contained, header-only RLE compression codec for DDP pixel data streams. Zero WLED core changes; this is a building block for compressed DDP support.

## What it does

- **Delta encoding**: XOR against previous frame — only changed pixels transmitted
- **Run-length encoding**: repeated byte values compressed to (count, value) pairs
- **Combined delta+RLE**: delta first, then RLE on the changed-pixel stream
- **Keyframe support**: periodic full frames to recover from packet loss
- **Transform mode**: uniform color operations + sparse explicit pixel writes

## Design constraints

- Header-only, zero external dependencies
- No heap allocation in the encode/decode hot path
- Fixed-size output buffer (caller-provided)
- `RLEDecoder` operates directly on the received packet buffer
- Safe for ESP32 ISR/callback contexts (no malloc, no locks)

## Compression ratios (measured)

| Pattern | Ratio | Bandwidth (800 LEDs, 40fps, 1.5Mbaud PPP) |
|---------|-------|--------------------------------------------|
| Rainbow (worst case) | 0% | 93.7 KB/s |
| Solid color pulse | 13-21% | 77.8 KB/s |
| Sparse twinkle (2% change) | 95% | 4.6 KB/s |

## Files changed

- `wled00/ddp_compress.h` (new) — `RLEDecoder`, compression helper functions

## Testing

- Codec tested via round-trip unit tests in `tools/tests/`. Deployed on M5StickC over 1.5Mbaud PPP link for 72+ hours continuous streaming.
- Fuzz-tested with random payloads for encoder/decoder round-trip correctness

## Dependencies

Note: `rle_encode_adaptive()` references `DDP_COMP_TYPE_*` constants defined in `ESPAsyncE131.h`. This header is modified in the companion PR `pr/ddp-compressed-receiver`. Submit this PR first, then `pr/ddp-compressed-receiver` which adds the constants and the receiver integration.

Subsequent PRs build on this:
1. `pr/ddp-compressed-receiver` — receiver-side decompression in `handleDDPPacket()`
2. `pr/ddp-compressed` — full stack including tools and protocol spec
## Related upstream issues

| Issue/PR | Repo | Title | Relevance |
|----------|------|-------|-----------|
| [#5755](https://github.com/wled/WLED/issues/5755) | Aircoookie/WLED | DDP-over-WebSocket fragmented packets | DDP bandwidth is a real constraint; compression directly addresses this |
| [#5412](https://github.com/wled/WLED/issues/5412) | Aircoookie/WLED | DDP out-of-sequence packets on WiFi | Compression reduces packet count, reducing reorder probability |
| [#4320](https://github.com/wled/WLED/issues/4320) | Aircoookie/WLED | UI unresponsive during DDP realtime | Compression reduces CPU load from packet processing |
| [PR #5554](https://github.com/wled/WLED/pull/5554) | Aircoookie/WLED | DDP improvements (merged) | Prior art — this codec extends the approach |
| [PR #5547](https://github.com/wled/WLED/pull/5547) | Aircoookie/WLED | DDP sequence handling (merged) | Sequence handling this codec builds on |
| [PR #5774](https://github.com/wled/WLED/pull/5774) | Aircoookie/WLED | Split udp.cpp into per-protocol files (open) | Coordinate timing — this PR touches the same file |
