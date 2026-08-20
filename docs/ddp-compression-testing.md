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
