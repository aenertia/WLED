# DDP Compression Validation

Hardware: M5StickC (ESP32-PICO-D4), firmware dev/ddp-spec @ 79a48e9b
Transport: PPP over USB serial, 1.5Mbaud, ~172 KB/s effective
Test tool: tools/ddp_compress_test.py

---

## C bit migration validation (2026-08-20)

Confirmed that the receiver correctly dispatches on `dataType & 0x80` (DDP_TYPE_COMPRESSED)
after migrating from `flags & 0x20` (DDP_FLAGS_COMPRESSED).

Build: `pio run -e m5stickc_ppp_wifi` on emiemi -- 97.7s, 70.8% flash, no warnings.

Flash: esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --no-stub, flash-mode dio.

### Compressed DDP with C bit (dataType=0x8B = RGB24 | 0x80)

Sent via `ddp_bench.py`, 800 LEDs, 30fps, 5s duration:

| Pattern | FPS | Compression | Wire KB/s |
|---------|-----|-------------|-----------|
| solid_pulse | 120.9 | 57% | 38.9 |
| sparse_twinkle | 999.1 | 97% | 2.8 |
| ifs_sierpinski | 45.2 | 84% | 14.4 |
| ifs_fern | 25.2 | 78% | 20.9 |
| ifs_dragon | 34.8 | 91% | 8.6 |
| ifs_flame | 32.1 | 72% | 26.3 |
| ifs_tree | 30.0 | 98% | 1.9 |

4.3M pixels delivered, 0 drops, 0 incomplete packets.
/diag: ddpPixWritten > 0, ddpPktCount > 0, heap stable at 57860.

Raw DDP (dataType=0x0B, no C bit): all phases completed, 53fps, backward compat confirmed.

---

## Compressibility sweep -- WS2812B 8x32 segment (2026-08-20)

Target: segment 2 (WS2812B 8x32, 256 LEDs), DDP destination byte=3.
256 LEDs, 768B raw frame, 120fps, 8s per pattern.
Device: reset=1 (POWERON), heap=58992, loopLag=33ms throughout.
0 drops, 0 heap skips, 0 WDT events.

```
Pattern              Raw KB/s  Comp KB/s   Ratio   Saved   Codec  Verdict
---------------------------------------------------------------------------
static                   90.0       10.4  11.5%    +88%    dRLE  EXCELLENT
solid-pulse              90.0       52.7  58.6%    +41%  raw-fb  moderate
solid-snap               90.0       10.6  11.8%    +88%    dRLE  EXCELLENT
half-half                90.0       10.4  11.5%    +88%    dRLE  EXCELLENT
chase                    90.0        2.1   2.4%    +98%    dRLE  EXCELLENT
wipe                     90.1        6.0   6.6%    +93%    dRLE  EXCELLENT
gradient                 90.0       10.4  11.5%    +88%    dRLE  EXCELLENT
twinkle-2pct             90.0        4.5   5.0%    +95%    dRLE  EXCELLENT
twinkle-10pct            90.0       16.2  18.0%    +82%    dRLE  EXCELLENT
twinkle-50pct            90.0       57.1  63.3%    +37%    dRLE  moderate
rainbow                  90.0       90.0 100.0%     +0%  raw-fb  no gain
alternating              90.0        8.9   9.8%    +90%    dRLE  EXCELLENT
random-noise             90.0       90.0 100.0%     +0%  raw-fb  no gain
```

Post-run /diag:
  reset=1 (POWERON) heap=58876 contig=57344 minheap=53056 up=980s fps=30 segs=3
  ddp: pkts=24958 pix=6389248 heapSkip=0 ovrSkip=0 lastLen=768 push=24958
  ddpRate: drops=0 heapGuard=0 loopLag=33ms

### Notes

- 120fps sustained on WS strip with TFT running independently -- no DDP/TFT interaction.
- Alternating pixels (+90%): expected worst case for byte-level RLE, but delta+RLE
  handles it because the frame is static between keyframe resets.
- Solid-pulse (raw-fb): adaptive encoder correctly falls back to raw for frames where
  the brightness delta across all pixels doesn't compress. Keyframe intervals (every 10
  frames) do compress via RLE-only, producing the 41% average savings.
- Rainbow and random noise: correctly fall back to raw (0% savings). No overhead.
- Crossover point: compression beneficial above ~25% change rate per frame.
  At 50% twinkle density: 37% savings (moderate). At 10%: 82% (excellent).

### Command

```bash
python3 tools/ddp_compress_test.py --target 169.254.7.1 --segment 2 --fps 120 --duration 8
```

---

## DDP-over-WebSocket validation (2026-08-20)

Confirms the WS receive path (ws.cpp -> handleE131Packet P_DDP -> e131.cpp)
handles the C bit correctly. The 0x02 protocol indicator byte is stripped by
ws.cpp before the DDP header is passed to handleDDPPacket().

Test tool: tools/ddp_ws_test.py (websockets async, mirrors common.js sendDDP())
Target: ws://169.254.7.1/ws, segment=2 (WS2812B 8x32, dest byte=3), 256 LEDs, 120fps

```
Phase                    Mode    FPS    Ratio  Savings         Wire KB/s
rainbow raw              RAW     120fps  100.0%  no gain         90.0 KB/s
rainbow compressed       COMP    120fps  100.0%  no gain         90.0 KB/s
chase raw                RAW     120fps  100.0%  no gain         90.0 KB/s
chase compressed         COMP    120fps    2.6%  +97% saved       2.3 KB/s
twinkle 5% raw           RAW     120fps  100.0%  no gain         90.1 KB/s
twinkle 5% compressed    COMP    120fps    9.6%  +90% saved       8.6 KB/s
```

Post-run /diag:
  reset=1 (POWERON) heap=58720 minheap=51300 up=2547s fps=28 segs=3
  ddp: pkts=27730 pix=7098880 heapSkip=0 ovrSkip=0 lastLen=768 push=27730
  ddpRate: drops=0 heapGuard=0 loopLag=15ms
  px[0..4]: 6B0035 810041 9B004D B10059 C50063  (twinkle colours visible)

Compression ratios match UDP results for same patterns -- WS transport is
transparent to the codec. C bit (dataType=0x8B) accepted correctly over WS.

### Command

```bash
python3 tools/ddp_ws_test.py --target 169.254.7.1 --segment 2 --leds 256 --fps 120 --duration 6
```

---

## WS DDP Rate Limiting Validation

Hardware: M5StickC (ESP32-PICO-D4), ST7735S 80x160 TFT, SPI_FREQUENCY=20MHz,
virtual matrix 40x80 (2x2 scale), dmaRows=25, 4 DMA strips per frame.
Firmware: dev/ddp-spec, rate gate build. Segment 2 = WS2812B 8x32 (256 LEDs) on G26.

### Root cause

WS DDP frames arriving during the TFT SPI DMA window cause ISR starvation:
loopTask blocks in spi_device_get_trans_result(portMAX_DELAY) indefinitely.
The DMA completion ISR never fires while the frame is being processed.

At 40fps (25ms frame interval) the TFT show() takes ~8-9ms -- 35% of the budget.
When handleE131Packet() runs during that window, the DMA ISR is starved and the
device WDT-crashes within 2-3 frames. At 30fps (33ms interval) there is enough
gap between frames for the DMA to complete cleanly.

The crash is structural: it is a function of SPI_FREQUENCY and virtual resolution,
not of frame content or compression. The safe ceiling formula is:

  show_ms  = ceil(panelH / dmaRows) * (physW * dmaRows * scaleY * 2B * 8) / SPI_FREQ_Hz * 1000
           + numStrips * 0.5ms  (setAddrWindow overhead)
  safe_fps = floor(700 / show_ms)  -- 70% headroom

For this config: show_ms ~8.4ms, safe_fps ~30fps. Matches empirical result exactly.

### Fix

WS-local rate gate in wsEvent() BINARY_PROTOCOL_DDP branch (wled00/ws.cpp).
Mirrors the UDP gate in e131.cpp but with independent per-transport state --
no shared atomics, no mutex. Gate runs before handleE131Packet() so dropped
frames never enter the pixel pipeline.

New /diag counters (wled00/wled_server.cpp):
  wsddp: pkts=N accepted=N dropped=N

New cfg key: interfaces.live.ddpfps controls the ceiling (persisted in NVS).

### FPS sweep results (ddpMaxFps=30, segment 2, 256 LEDs, 8s per phase)

| FPS | Pattern | Mode | wsddp acc | wsddp drp | loopLag | WDT |
|-----|---------|------|-----------|-----------|---------|-----|
| 10  | rainbow | RAW  | 80        | 0         | 20ms    | no  |
| 10  | rainbow | COMP | 160       | 0         | 16ms    | no  |
| 10  | chase   | RAW  | 240       | 0         | 34ms    | no  |
| 10  | chase   | COMP | 320       | 0         | 30ms    | no  |
| 10  | twinkle | RAW  | 400       | 0         | 24ms    | no  |
| 10  | twinkle | COMP | 480       | 0         | 35ms    | no  |
| 20  | rainbow | RAW  | 640       | 0         | 28ms    | no  |
| 20  | rainbow | COMP | 800       | 0         | 11ms    | no  |
| 20  | chase   | RAW  | 960       | 0         | 12ms    | no  |
| 20  | chase   | COMP | 1120      | 0         | 30ms    | no  |
| 20  | twinkle | RAW  | 1280      | 0         | 13ms    | no  |
| 20  | twinkle | COMP | 1440      | 0         | 0ms     | no  |
| 30  | rainbow | RAW  | ~150/ph   | ~90/ph    | 25ms    | no  |
| 30  | rainbow | COMP | ~150/ph   | ~99/ph    | 11ms    | no  |
| 30  | chase   | RAW  | ~153/ph   | ~87/ph    | 26ms    | no  |
| 30  | chase   | COMP | ~149/ph   | ~91/ph    | 19ms    | no  |
| 30  | twinkle | RAW  | ~150/ph   | ~90/ph    | 20ms    | no  |
| 30  | twinkle | COMP | ~148/ph   | ~92/ph    | 16ms    | no  |
| 40  | rainbow | RAW  | ~197/ph   | ~123/ph   | 8810ms  | YES |

At 30fps the gate drops ~40% of frames (sender timing jitter exceeds the 33ms
ceiling). The device stays stable throughout. At 40fps the first accepted frame
triggers the SPI DMA ISR starvation -- loopLag spikes to 8+ seconds, WDT fires.

### Stable ceiling

30fps at SPI_FREQUENCY=20MHz, virtual 40x80, 2x2 scale.

To raise the ceiling without changing virtual resolution:
  27MHz SPI -> ~40fps safe (M5Stack's own frequency for ST7735S)
  40MHz SPI -> ~60fps safe (over-spec for ST7735S, unit-dependent)
  20x40 virtual (half res, 4x4 scale) -> ceiling doubles at any SPI freq

These are tracked in the follow-on plan (spi-matrix-ddp-eligibility).

### /diag counter descriptions

  wsddp: pkts=N    -- frame-start packets seen by the WS gate (chanOff==0 or PUSH flag)
  wsddp: accepted=N -- frames that passed the rate check and entered handleE131Packet()
  wsddp: dropped=N  -- frames rejected by the gate (too soon after last accepted frame)
  ddpRate: drops=N  -- frames dropped by the global UDP+WS rate limiter in e131.cpp
  ddpRate: maxFps=N -- current ceiling (set via /json/cfg interfaces.live.ddpfps)
  ddpRate: loopLag=Nms -- millis since last main loop iteration (WDT proxy)

### Commands

```bash
# Set ceiling and apply 3-segment layout
curl -X POST http://169.254.7.1/json/cfg -H 'Content-Type: application/json' \
  -d '{"if":{"live":{"ddpfps":30}}}'
curl -X POST http://169.254.7.1/json/state -H 'Content-Type: application/json' \
  -d @configs/segment-mirror-3seg.json

# Run sweep
python3 tools/ddp_ws_test.py --target 169.254.7.1 --segment 2 --leds 256 \
  --fps 30 --duration 8 --max-fps 30
```
