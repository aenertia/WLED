# feat(ddp): compressed DDP — full stack (codec + receiver + tools + spec)

**Forgejo**: Fixes #16

## Summary

Complete implementation of compressed DDP for WLED — header-only RLE codec, receiver-side decompression in `handleDDPPacket()`, Python benchmark/codec tools, and protocol specification. Designed for bandwidth-constrained links (PPP serial, slow WiFi) where standard DDP saturates the transport.

## What it does

### Codec (`ddp_compress.h`)
- **Delta encoding**: XOR against previous frame — only changed pixels transmitted
- **Run-length encoding**: repeated byte values compressed to (count, value) pairs
- **Combined delta+RLE**: delta first, then RLE on the changed-pixel stream
- **Transform mode**: uniform color operations + sparse explicit pixel writes
- **Keyframe support**: periodic full frames to recover from packet loss

### Receiver (`e131.cpp`)
When `WLED_ENABLE_DDP_COMPRESSION` is defined and DDP flag bit 5 is set:
1. Decodes RLE-compressed pixel stream via `RLEDecoder`
2. Applies delta decoding against previous frame buffer (`ddpPrevFrame`, RGB565)
3. Writes reconstructed pixels via `setRealtimePixel()`

Compression type signalled in upper nibble of DDP sequence number:
- `0x10` — delta+RLE (most frames)
- `0x20` — RLE only (keyframes)
- `0x30` — transform mode

### Tools
- `tools/ddp_bench.py` — benchmark tool with `--compress delta_rle|rle|transform` support
- `tools/ddp_codec.py` — standalone Python codec library for DDP senders

### Protocol spec
- `docs/ddp-readme.md` — compression extension specification

## Compression ratios (measured, 800 LEDs, 40fps, 1.5Mbaud PPP)

| Pattern | Ratio | Bandwidth |
|---------|-------|-----------|
| Rainbow (worst case) | 0% | 93.7 KB/s |
| Solid color pulse | 13-21% | 77.8 KB/s |
| Sparse twinkle (2% change) | 95% | 4.6 KB/s |

## Files changed

- `wled00/ddp_compress.h` (new) — header-only RLE codec
- `wled00/e131.cpp` (modified) — receiver-side decompression
- `wled00/src/dependencies/e131/ESPAsyncE131.h` (modified) — compression flag constants, enlarged packet buffer
- `tools/ddp_bench.py` (new) — benchmark tool with compression support
- `tools/ddp_codec.py` (new) — Python codec library
- `docs/ddp-readme.md` (new) — protocol specification

## Testing

- Codec round-trip tested via `ddp_codec.py` with random payloads
- Receiver tested with `ddp_bench.py --compress delta_rle` sending compressed streams
- Backwards compatibility verified: standard DDP senders work unchanged
- 72+ hours continuous compressed streaming on M5StickC over 1.5Mbaud PPP
- Transform mode tested with fade-to-black and scale operations

## Split PRs available

This is the combined PR. Individual PRs for easier review:
1. `pr/ddp-rle-codec` — codec only (`ddp_compress.h`)
2. `pr/ddp-compressed-receiver` — codec + receiver (`e131.cpp` changes)