# DDP Implementation Reference

**Repo**: `git@git.awa.3d.ae.net.nz:aenertia/wled.git`, branch `dev/ddp-spec`
**Device**: M5StickC (ESP32-PICO-D4, 4MB flash, no PSRAM), 40x80 virtual matrix
**Transport tested**: PPP 1.5Mbaud, WiFi UDP

This document describes the DDP implementation in this fork. It covers the wire
format, compression extension, per-segment routing, receiver behaviour, and
measured performance on real hardware. Everything here has been tested on device.

---

## 1. Base protocol

Source: http://www.3waylabs.com/ddp/
Port: UDP 4048 unicast.

### 1.1 Header (10 bytes)

```
Byte 0:   flags       [VV000TPQ]  VV=01 (version), T=timecode, P=push, Q=query
Byte 1:   sequenceNum [CCCCSSSS]  C=compType upper nibble, S=seq lower nibble 1-15
Byte 2:   dataType    [C0TTTBBB]  C=0x80 compressed flag, TTT=type, BBB=bits/ch
Byte 3:   destination             0=default, 1-32=segment ID in this fork
Bytes 4-7: channelOffset          byte offset into pixel buffer, big-endian uint32
Bytes 8-9: dataLen                payload length in bytes, big-endian uint16
Byte 10+: payload
```

### 1.2 Flags (byte 0)

| Bit | Name  | Meaning |
|-----|-------|---------|
| 7-6 | VV    | Version -- must be 0x40 |
| 4   | T     | Timecode present (4 bytes added after header) |
| 3   | S     | Storage -- data from local storage, not packet |
| 2   | R     | Reply |
| 1   | Q     | Query |
| 0   | P     | Push -- render now, last packet of frame |

### 1.3 Sequence number (byte 1)

Lower nibble (bits 3-0): sequence, values 1-15. Wraps 15 -> 1. Value 0 disables
sequence filtering. Must increment across all packets in a frame and across frames.

Upper nibble (bits 7-4): compression type when dataType C bit is set (see section 3).

Using a fixed sequence number causes silent drops after the first multi-packet frame.

### 1.4 Data type (byte 2)

Bit 7 (C bit, 0x80): compressed payload. The lower 7 bits retain normal dataType
meaning. A compressed RGB packet has dataType = 0x8B (0x80 | 0x0B).

Common values:

| Value | Meaning |
|-------|---------|
| 0x0B  | RGB24 -- 8 bits/channel, 3 channels |
| 0x1B  | RGBW32 -- 8 bits/channel, 4 channels |
| 0x01  | Legacy RGB (accepted, treated as RGB24) |

RGBW detection: `(dataType & 0b00111000) >> 3 == 0b011`.

### 1.5 Channel offset

Byte offset into the pixel buffer, not a pixel index. Pixel index = offset / channels.
For multi-packet frames each packet carries the byte offset of its first byte.

### 1.6 Push flag

Set on the last packet of a frame. Triggers `strip.show()` via `e131NewData`.
Non-push packets write into `seg.pixels[]` but do not trigger render.

The receiver only sets `e131NewData` on PUSH packets. Legacy senders that never
set PUSH are not supported for multi-packet frames.

---

## 2. Per-segment routing (WLED extension)

The destination byte (byte 3) is repurposed for sub-device segment routing.

### 2.1 Mode A -- destination-routed

destination 1-32 routes to segment N-1. channelOffset is segment-relative (0 =
first pixel of that segment). The segment must be active; out-of-range is dropped.

```python
# Route to segment 1 (id=0): destination=1
struct.pack("!BBBBIH", DDP_VER1|DDP_PUSH, seq, DDP_RGB, 1, 0, len(payload))
```

### 2.2 Mode B -- eligibility mask

destination=0 with `ddpEligibleMask` non-zero. The sender streams a flat pixel
buffer; the receiver distributes it across eligible segments in segment-index order
using a pre-computed slot table (`ddpSlots[]`, `ddpSlotCount`).

Set mask via `/json/cfg`: `{"if":{"live":{"ddpelig":3}}}` (segments 0 and 1).

The slot table is rebuilt when:
- `/json/cfg` POST changes `ddpelig`
- `/json/state` POST adds or modifies segments (after `strip.resume()`)
- `beginStrip()` completes at boot

`/diag` exposes `ddpSlots=N totalElig=M eligMask=0x...` for verification.

### 2.3 Legacy

destination=0 with `ddpEligibleMask=0` -- full-strip absolute pixel indexing.

---

## 3. Compression extension

### 3.1 Wire encoding

For compressed frames:
- Set dataType bit 7 (0x80): `dataType = (normal_type) | 0x80`
- Set sequenceNum upper nibble to compression type: `seq_byte = (comp_type & 0xF0) | (seq & 0x0F)`

### 3.2 Compression types

| Code | Name | Multi-packet | Notes |
|------|------|-------------|-------|
| 0x00 | Raw RGB | yes | No C bit, standard DDP |
| 0x10 | Delta+RLE | yes | XOR delta then byte-level PackBits |
| 0x20 | RLE | yes | Byte-level PackBits, no delta |
| 0x30 | Transform | yes | Uniform op + sparse explicit writes |
| 0x40 | Delta-only | yes | Raw XOR delta, no RLE -- benchmark mode |
| 0x50 | Tuple-RLE | **single-packet only** | Pixel-unit PackBits (see §3.7) |
| 0x60 | Planar-RLE | **single-packet only** | Per-channel planes (see §3.8) |

### 3.3 RLE codec (byte-level PackBits)

Used by 0x10, 0x20, and 0x40. Operates on the raw byte stream.

Control byte encoding:
- bit 7 = 0: RUN -- next byte repeated `(ctrl & 0x7F) + 1` times (1-128)
- bit 7 = 1: LITERAL -- next `(ctrl & 0x7F) + 1` bytes verbatim (1-128)

Worst-case expansion: `srcLen + ceil(srcLen / 128)` bytes (~0.8%).

### 3.4 Delta+RLE (0x10)

1. XOR current frame against previous frame (stored as RGB565 on device, 2B/pixel).
2. Apply byte-level RLE to the XOR delta stream.
3. Receiver: RLE decode, XOR against stored prevFrame, write pixels.

prevFrame is allocated on first compressed packet, freed on `exitRealtime()`.
With single-segment DDP, allocation is scoped to the segment length, not the whole
strip (~3.2KB for 1600px vs ~6.4KB for 3200px).

Desync recovery: send a 0x20 keyframe to reset prevFrame to zeros on both sides.

### 3.5 RLE keyframe (0x20)

Byte-level RLE of the raw pixel data, no delta. When received with channelOffset=0,
the receiver zeros its prevFrame buffer. Use for stream restart or first frame.

### 3.6 Delta-only (0x40)

Raw XOR delta bytes, same size as uncompressed frame. Useful for measuring delta
entropy without RLE overhead. Not useful in production -- 0x10 always wins or ties.

### 3.7 Tuple-RLE (0x50) -- single-packet only

Same PackBits control bytes as 0x20, but the unit is one full pixel
(3 bytes RGB or 4 bytes RGBW) instead of one byte.

**Single-packet constraint**: the device decoder initialises `pixel = channelOffset /
channels` at the start of each packet. Continuation packets (channelOffset > 0) are
treated as new stream starts and decode garbage. The entire compressed frame MUST
fit in one UDP packet (<= MTU - 10, typically <= 1452B).

Practical limit: only viable when content compresses to <1452B. Twinkle-rainbow at
1600px encodes to ~4700B -- does not fit. Solid colours and palette wipes at 40x40
typically fit.

Validated: glitches confirmed on multi-packet Tuple-RLE frames (session 27).

### 3.8 Planar-RLE (0x60) -- single-packet only

Per-channel plane encoding:

```
[R_len: 2 bytes LE][R_rle_data: R_len bytes]
[G_len: 2 bytes LE][G_rle_data: G_len bytes]
[B_len: 2 bytes LE][B_rle_data: B_len bytes]
```

Each plane is byte-level PackBits-encoded independently. Plane headers are only
valid at channelOffset=0. Continuation packets (channelOffset > 0) are silently
ignored. Frame MUST fit in a single UDP packet (<= ~1452B).

Best codec for solid/uniform content -- each channel compresses as a single run.
Incompressible for rainbow or noise content (per-plane entropy is still high).

---

## 4. Receiver behaviour

### 4.1 Packet validation

Reject if:
- packetLen < 10
- destination is CONTROL(246), STATUS(251), or CONFIG(250)
- QUERY or REPLY flags set
- STORAGE flag set without PUSH
- packetLen < header + timecode_offset + dataLen

Out-of-sequence rejection (when `e131SkipOutOfSequence` enabled):
```c
int sn = p->sequenceNum & 0x0F;
if (sn && lastPushSeq) {
    if (lastPushSeq > 5) {
        if (sn > (lastPushSeq-5) && sn < lastPushSeq) return; // late
    } else {
        if (sn > (10+lastPushSeq) || sn < lastPushSeq) return; // late (wrapped)
    }
}
```

### 4.2 Realtime mode entry

Every DDP packet (not just PUSH) calls `realtimeLock(realtimeTimeoutMs, REALTIME_MODE_DDP)`.
Default timeout: 2500ms. Configurable via `/json/cfg` (not exposed in UI).

On mode entry:
- If `ddpSlotCount > 0` and dest < 1: `freezeEligibleSegs()` -- clears and freezes
  each eligible segment's pixel buffer, sets `rtFrozenSegs` bitmask.
- If dest 1-32: `freezeSegForRealtime(dest-1)` -- same for the specific segment.
- If `rtFrozenSegs == 0 && ddpSlotCount == 0`: legacy path, `strip.fill(BLACK)`.

### 4.3 Show path with frozen segments

When `rtFrozenSegs` is non-zero and non-frozen segments (e.g. effects) are also
active (Case D in `showFrozenSegs()`):

- `service()` runs effect functions normally but does NOT push to bus (`!rtFrozenSegs` gate).
- `handleNotifications()` calls `showFrozenSegs()` on PUSH cadence via `e131NewData`.
- `showFrozenSegs()` Case D calls `show()`, which blends all segments from their
  `seg.pixels[]` buffers -- effect data for non-frozen segs, DDP data for frozen segs
  -- then `BusManager::show()` pushes to the bus atomically.

This prevents the 42fps `service()` show from racing DDP's multi-packet write.
Validated: session 27, Ghost Rider seg0 + DDP twinkle seg1, all codecs tested.

### 4.4 Rate limiter

Global rate limiter gates all DDP packets regardless of transport. Per-frame:
- Frame start detected by `channelOffset == 0` or PUSH flag.
- `effFps = min(ddpMaxFps, ddpCurrentSafeFps)` (0 = unlimited).
- `minIntervalUs = 1000000 / effFps`.
- If elapsed < minIntervalUs at frame start: set `ddpDropCurrentFrame = true`,
  drop all subsequent packets in that frame.

`ddpCurrentSafeFps` is computed each main loop iteration from `BusManager::computeSafeDdpFps()`:
sum of `bus.getShowUs()` across active buses, 70% headroom applied.

On M5StickC (40x80 TFT only, no WS strip): `ddpSafe=103fps, sumUs=6740`.
With WS strip idle: same (skip-show gates out the WS show time).
With WS strip active (256px): `ddpSafe=~34fps, sumUs=~14420`.

### 4.5 Heap guard

If `esp_get_free_heap_size() < 20000`: all DDP dropped (heap guard).
If `ESP.getFreeHeap() < DDP_MIN_HEAP (10240)`: pixel writes skipped, PUSH still
processed (frame completes, show fires, but with zeros).

### 4.6 /diag fields

```
mode=N          -- realtimeMode (8=REALTIME_MODE_DDP, 0=inactive)
override=N      -- realtimeOverride
timeout=T       -- realtimeTimeout (millis absolute)
now=T diff=D    -- millis(), diff=now-timeout (negative=still active)
frozen=0x...    -- rtFrozenSegs bitmask
ddpSlots=N      -- slot table entries (Mode B)
totalElig=M     -- total eligible pixels (sum of slot lengths)
eligMask=0x...  -- ddpEligibleMask
ddp: pkts=N pix=N heapSkip=N ovrSkip=N lastStart=N lastLen=N push=N
ddp2: incomplete=N passed=N lastPktLen=N lastClaimed=N
ddpRate: drops=N heapGuard=N maxFps=N loopLag=Nms
ddpSafe: fps=N sumUs=N
bus[N].showUs=N skip=N
px[0..4]: RRGGBB ...   -- _pixels[] values (zeros during frozen-seg DDP; use frozen= to confirm)
```

Note: `px[]` reads from `_pixels[]`, which is zeroed by the fast-path show. The bus
has correct pixel data even when `px[]` shows 0x000000.

---

## 5. Sender guide

### 5.1 Raw single-packet frame

```python
import socket, struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_raw(ip, pixels_rgb, seq=1):
    data = bytes(pixels_rgb)
    pkt = struct.pack("!BBBBIH",
        0x41,       # VER1 | PUSH
        seq & 0x0F,
        0x0B,       # RGB24
        0,          # destination (Mode B or legacy)
        0,          # channelOffset
        len(data))
    sock.sendto(pkt + data, (ip, 4048))
```

### 5.2 Multi-packet frame

```python
def send_frame(ip, data, comp_type=0x00, data_type=0x0B, destination=0):
    off = 0; seq = 1
    if comp_type != 0x00:
        data_type |= 0x80  # set C bit
    while off < len(data):
        chunk = min(1200, len(data) - off)
        last  = (off + chunk) >= len(data)
        flags = 0x40 | (0x01 if last else 0)
        seq_byte = (comp_type & 0xF0) | (seq & 0x0F)
        pkt = struct.pack("!BBBBIH", flags, seq_byte, data_type,
                          destination, off, chunk)
        sock.sendto(pkt + data[off:off+chunk], (ip, 4048))
        off += chunk
        seq = (seq % 15) + 1
```

### 5.3 RLE encoder

```python
def rle_encode(data):
    out = bytearray(); i = 0
    while i < len(data):
        v = data[i]; run = 1
        while run < 128 and (i+run) < len(data) and data[i+run] == v:
            run += 1
        if run > 1:
            out.append(run - 1); out.append(v); i += run
        else:
            ls = i
            while i < len(data):
                if (i+1) < len(data) and data[i] == data[i+1]: break
                i += 1
                if (i - ls) == 128: break
            out.append(0x80 | (i - ls - 1)); out.extend(data[ls:i])
    return bytes(out)
```

### 5.4 Delta+RLE encoder

The device prevFrame uses RGB565 packing. The sender must mirror this or desync
occurs on codec switch. On codec switch: send a 0x20 keyframe first.

```python
def rgb_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def unpack_565(v):
    return (v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8

prev_565 = [0] * N  # per-sender state, N = pixel count

def encode_delta_rle(rgb):
    delta = bytearray(N * 3)
    for i in range(N):
        pr, pg, pb = unpack_565(prev_565[i])
        delta[i*3]   = rgb[i*3]   ^ pr
        delta[i*3+1] = rgb[i*3+1] ^ pg
        delta[i*3+2] = rgb[i*3+2] ^ pb
        prev_565[i] = rgb_to_565(rgb[i*3], rgb[i*3+1], rgb[i*3+2])
    return rle_encode(bytes(delta))
```

### 5.5 Codec selection

| Content | Best codec | Ratio (PPP 1.5Mbaud, 3200px) | Max fps |
|---------|-----------|------------------------------|---------|
| Rainbow / gradient | 0x00 raw | 1.0x | 10fps (link-limited) |
| Twinkle-rainbow (1600px WiFi) | 0x20 RLE | ~0.96x | 18-22fps |
| Sparse twinkle (<10% change) | 0x10 delta+RLE | 0.12x | 20fps+ |
| Ghost rider / moving particle | 0x20 RLE | 0.032x | 120fps |
| Solid / uniform colour | 0x60 planar-RLE | 0.016x | 120fps |
| Palette wipe (pixel runs) | 0x50 tuple-RLE | varies | single-pkt only |
| First frame / resync | 0x20 RLE | varies | -- |

Source: session 25 PPP sweep (3200px, 40x80), session 27 WiFi sweep (1600px, 40x40).

---

## 6. Performance

### 6.1 PPP 1.5Mbaud (session 25, 3200px, 40x80)

```
Codec        Pattern       Smooth fps  Wire KB/s  Ratio  Drops
--------------------------------------------------------------
raw          rainbow          10fps       93.7   1.000      0
rle          ghost_rider     120fps       35.7   0.032      0
delta-rle    twinkle          20fps       23.5   0.125      0
delta-rle    ghost_rider     120fps       36.8   0.033      0
planar-rle   pulse           120fps       18.3   0.016      0
```

Link ceiling at 124 KB/s (70% of 1.5Mbaud nominal): raw rainbow hits it at 10fps.
drops=0 across 16 codec x pattern cells. heapGuard=0 throughout.

### 6.2 WiFi UDP (session 27, 1600px, 40x40, twinkle-rainbow)

```
Codec        fps   wire B/f  ratio  drops
------------------------------------------
Raw RGB      21.4    4840    1.008      2
RLE          17.9    4663    0.971     54  (drops from WiFi show-guard pacing)
Delta+RLE    18.9    4690    0.977     31
Tuple-RLE    20.9    4742    0.988      3  (visual glitch -- multi-pkt decoder bug)
Planar-RLE   --      0       --         0  (all frames exceed MTU, skipped)
```

Twinkle-rainbow is essentially incompressible with all codecs. The WiFi show-guard
(15ms minimum between shows) caps achieved fps below the 30fps target.

### 6.3 WiFi UDP (session 27, visual validation, Ghost Rider seg0 + DDP seg1)

After `service()` show-race fix (ff55be3d):

| Codec | Result |
|-------|--------|
| Raw RGB | clean |
| RLE (0x20) | clean |
| Delta+RLE (0x10) | clean |
| Tuple-RLE (0x50) | glitch (multi-packet decoder limitation) |

### 6.4 Heap (M5StickC, m5stickc_ppp_wifi build)

```
Boot heap (post all fixes): ~67KB free
minheap during DDP (PPP sustained, delta-rle, 3200px): ~46KB
  breakdown: prevFrame=6.4KB, udpIn=1.5KB, lwIP pbufs=2.5KB, JSON=0.7KB
heapGuard (20KB threshold): never triggered
50x realtime enter/exit soak: delta=0B (no leak)
```

### 6.5 Wave 3 regression baselines

Branch: dev/ddp-spec @ bece94c3, M5StickC 40x80 TFT:

```
UDP WiFi:  661fps rainbow raw, 997fps pulse raw, heapGuard=0
UDP PPP:   41.8fps rainbow raw (link-limited), 435fps twinkle compressed, heapGuard=0
WS WiFi:   150fps ceiling (256px segment), heapGuard=0
WS PPP:    150fps compressed (256px segment), heapGuard=0
```

---

## 7. Constants (ESPAsyncE131.h)

```c
#define DDP_FLAGS_VER1      0x40
#define DDP_FLAGS_PUSH      0x01
#define DDP_FLAGS_QUERY     0x02
#define DDP_FLAGS_REPLY     0x04
#define DDP_FLAGS_STORAGE   0x08
#define DDP_FLAGS_TIME      0x10

#define DDP_TYPE_COMPRESSED 0x80  // C bit: payload is compressed
#define DDP_TYPE_RGB24      0x0B  // RGB, 8 bits/channel
#define DDP_TYPE_RGBW32     0x1B  // RGBW, 8 bits/channel

#define DDP_COMP_TYPE_NONE       0x00
#define DDP_COMP_TYPE_DELTA_RLE  0x10  // XOR delta + PackBits RLE
#define DDP_COMP_TYPE_RLE        0x20  // PackBits RLE (keyframe)
#define DDP_COMP_TYPE_TRANSFORM  0x30  // uniform op + sparse writes
#define DDP_COMP_TYPE_DELTA_ONLY 0x40  // raw XOR delta, no RLE
#define DDP_COMP_TYPE_TUPLE_RLE  0x50  // pixel-unit PackBits (single-pkt)
#define DDP_COMP_TYPE_PLANAR_RLE 0x60  // per-channel planes (single-pkt)

#define DDP_DEFAULT_PORT    4048
#define DDP_HEADER_LEN      10
#define DDP_ID_CONTROL      0xF6
#define DDP_ID_CONFIG       0xFA
#define DDP_ID_STATUS       0xFB
```

---

## 8. Implementation files

| File | Purpose |
|------|---------|
| `wled00/e131.cpp` | `handleDDPPacket()` -- all decoders, routing, rate limiter, prevFrame |
| `wled00/udp.cpp` | `rebuildDdpSlots()`, `freezeEligibleSegs()`, `realtimeLock()`, `showFrozenSegs()` |
| `wled00/FX_fcn.cpp` | `showFrozenSegs()` Case D, `service()` show-gate (`!rtFrozenSegs`) |
| `wled00/wled.cpp` | `beginStrip()` post-call `rebuildDdpSlots()`, backup timeout check |
| `wled00/json.cpp` | `deserializeState()` post-resume `rebuildDdpSlots()` |
| `wled00/wled.h` | `ddpEligibleMask`, `ddpSlotCount`, `ddpTotalEligible`, `ddpSlots[]` |
| `wled00/wled_server.cpp` | `/diag` -- ddpSlots, eligMask, frozen, mode, px[] |
| `wled00/ddp_compress.h` | `RLEDecoder` streaming struct |
| `wled00/src/dependencies/e131/ESPAsyncE131.h` | DDP constants |
| `tools/ddp_bench.py` | Python sender: all codecs, sweep, diagnostic |
| `tools/ddp_codec.py` | Python codec reference implementations |

---

## 9. Known issues / not yet validated

- **Tuple-RLE (0x50) multi-packet**: decoder reinitialises pixel=start per packet.
  Only works when full compressed frame fits in one UDP payload. Glitch confirmed
  session 27 on 1600px twinkle content (~4700B encoded, 4 packets needed).

- **prevFrame RGB565 quantisation**: prevFrame stores pixels as 16-bit RGB565
  (3 LSBs stripped per channel). XOR delta accumulates ~1% quantisation error per
  frame. Invisible on LED displays; measurable in test fixtures.

- **Tuple-RLE (0x50) / Planar-RLE (0x60) tested PPP session 25** as single-packet
  (pulse/solid content). Multi-packet failure only confirmed WiFi session 27.

- **Per-segment DDP (Mode A, destination byte routing)**: implemented and unit-tested
  via /diag. Full end-to-end visual validation with explicit destination byte not
  done -- only Mode B (eligibility mask) validated end-to-end.

- **WebSocket DDP** (`ws.cpp`): implemented, not tested this branch.

- **Transform codec (0x30)**: implemented in `e131.cpp`, not benchmarked.
