#!/usr/bin/env python3
"""
DDP-over-WebSocket FPS performance test.

Measures maximum sustained FPS for raw vs compressed DDP on a specific segment.
Ramps through FPS targets, measures actual achieved FPS, drops, and heap impact.
Uses patterns that stress different parts of the codec:
  - chase: best-case compression (97% savings), tests codec overhead at high FPS
  - rainbow: worst-case compression (0% savings), raw fallback path
  - twinkle: mid-range compression (~90% savings)

Usage:
    python3 tools/ddp_ws_fps_test.py --target 169.254.7.1 --segment 2 --leds 256
"""

import sys, struct, time, random, argparse, asyncio, urllib.request
import websockets

DDP_VER1 = 0x40; DDP_PUSH = 0x01
DDP_TYPE_COMPRESSED = 0x80; DDP_RGB = 0x0B
COMP_NONE = 0x00; COMP_DELTA_RLE = 0x10; COMP_RLE = 0x20
WS_DDP = 0x02

def rle_encode(src):
    out, i, n = bytearray(), 0, len(src)
    while i < n:
        cur, run = src[i], 1
        while i+run < n and src[i+run] == cur and run < 128: run += 1
        if run >= 3:
            out.append(run-1); out.append(cur); i += run
        else:
            ls, ll = i, 0
            while i < n and ll < 128:
                a = 1
                while i+a < n and src[i+a] == src[i] and a < 3: a += 1
                if a >= 3: break
                i += 1; ll += 1
            if ll: out.append(0x80|(ll-1)); out.extend(src[ls:ls+ll])
    return bytes(out)

def compress_adaptive(cur, prev=None):
    best, btype = cur, COMP_NONE
    rle = rle_encode(cur)
    if len(rle) < len(best): best, btype = rle, COMP_RLE
    if prev and len(prev) == len(cur):
        delta = bytes(a^b for a,b in zip(cur, prev))
        drle = rle_encode(delta)
        if len(drle) < len(best): best, btype = drle, COMP_DELTA_RLE
    return best, btype

def make_ws_pkt(data, seq=1, comp=COMP_NONE, data_type=DDP_RGB, destination=0x01):
    flags = DDP_VER1 | DDP_PUSH
    dt = data_type | DDP_TYPE_COMPRESSED if comp != COMP_NONE else data_type
    hdr = struct.pack("!BBBBIH", flags, (seq&0x0F)|(comp&0xF0), dt, destination, 0, len(data))
    return bytes([WS_DDP]) + hdr + data

def gen_chase(n, t):
    d = bytearray(n*3)
    pos = int(t * n * 2) % n
    for i in range(max(1, n//16)):
        x = ((pos+i)%n)*3; d[x]=0; d[x+1]=220; d[x+2]=255
    return bytes(d)

def gen_rainbow(n, t):
    d = bytearray(n*3)
    for i in range(n):
        h = ((i/n)+t*3.0)%1.0*6; c=int(h); f=h-c; q=int(255*(1-f)); tv=int(255*f); x=i*3
        if c==0:   d[x],d[x+1],d[x+2]=255,tv,0
        elif c==1: d[x],d[x+1],d[x+2]=q,255,0
        elif c==2: d[x],d[x+1],d[x+2]=0,255,tv
        elif c==3: d[x],d[x+1],d[x+2]=0,q,255
        elif c==4: d[x],d[x+1],d[x+2]=tv,0,255
        else:      d[x],d[x+1],d[x+2]=255,0,q
    return bytes(d)

def gen_twinkle(n, t, prev=None):
    random.seed(int(t*1000))
    d = bytearray(prev) if prev else bytearray(n*3)
    for _ in range(max(1, int(n*0.05))):
        px=random.randint(0,n-1); x=px*3
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255); d[x+2]=random.randint(0,255)
    return bytes(d)

def get_diag(target):
    try:
        with urllib.request.urlopen(f"http://{target}/diag", timeout=3) as r:
            return r.read().decode()
    except:
        return ""

def parse_diag(diag):
    vals = {}
    for tok in diag.split():
        if '=' in tok:
            k, _, v = tok.partition('=')
            try: vals[k] = int(v.split('(')[0])
            except: vals[k] = v
    return vals

async def run_fps_level(ws, n, dest, gen_fn, compressed, target_fps, dur, uses_prev=False):
    """Run one FPS level, return (achieved_fps, wire_kbs, ratio, drops_detected)."""
    iv = 1.0/target_fps
    sent = 0; raw_b = 0; wire_b = 0; prev = None; seq = 1
    skipped = 0  # frames we couldn't send in time
    start = time.monotonic(); nxt = start

    while time.monotonic() - start < dur:
        now = time.monotonic()
        if now < nxt:
            await asyncio.sleep(max(0, nxt - now - 0.0002))
            continue
        # if we're more than one frame behind, skip (sender overrun)
        if now > nxt + iv:
            skipped += 1
            nxt = now
        t = (now - start) / dur
        px = gen_fn(n, t, prev) if uses_prev else gen_fn(n, t)
        raw_b += len(px)
        if compressed:
            cd, ct = compress_adaptive(px, prev)
            pkt = make_ws_pkt(cd, seq, comp=ct, destination=dest)
            wire_b += len(cd)
        else:
            pkt = make_ws_pkt(px, seq, destination=dest)
            wire_b += len(px)
        prev = px
        if sent % 10 == 0: prev = None
        try:
            await ws.send(pkt)
            sent += 1
        except Exception as e:
            return None, None, None, True
        seq = (seq % 15) + 1
        nxt += iv

    el = time.monotonic() - start
    afps = sent / el if el else 0
    ratio = wire_b / raw_b if raw_b else 1.0
    kbs = wire_b / 1024 / el if el else 0
    return afps, kbs, ratio, skipped > (target_fps * dur * 0.05)  # >5% skip = overrun

async def fps_sweep(ws, n, dest, label, gen_fn, compressed, fps_levels, dur, uses_prev=False):
    """Sweep through FPS levels, find the ceiling."""
    mode = "COMP" if compressed else "RAW "
    print(f"\n  [{label}] {mode}")
    print(f"  {'Target':>6}  {'Actual':>6}  {'Wire KB/s':>9}  {'Ratio':>6}  {'Status'}")
    print(f"  {'-'*6}  {'-'*6}  {'-'*9}  {'-'*6}  {'-'*20}")

    ceiling = None
    results = []
    for fps in fps_levels:
        afps, kbs, ratio, overrun = await run_fps_level(
            ws, n, dest, gen_fn, compressed, fps, dur, uses_prev)
        if afps is None:
            print(f"  {fps:>6}  {'WS ERROR':>6}")
            break
        pct = afps / fps * 100
        status = "OK" if pct >= 95 and not overrun else ("OVERRUN" if overrun else f"THROTTLED ({pct:.0f}%)")
        sv = f"{(1-ratio)*100:.0f}% saved" if ratio < 0.99 else "raw fallback"
        print(f"  {fps:>6}  {afps:>6.0f}  {kbs:>9.1f}  {ratio:>5.1%}  {status}  [{sv}]")
        results.append((fps, afps, kbs, ratio, overrun))
        if overrun and ceiling is None:
            ceiling = fps_levels[fps_levels.index(fps) - 1] if fps_levels.index(fps) > 0 else fps
        await asyncio.sleep(0.2)

    if ceiling is None and results:
        ceiling = results[-1][0]
    return ceiling, results

async def main_async(args):
    dest = args.segment + 1
    n = args.leds

    print("=" * 72)
    print("DDP-over-WebSocket FPS Performance Test")
    print(f"Target: ws://{args.target}/ws  segment={args.segment} dest={dest}  {n} LEDs")
    print(f"Raw frame: {n*3}B  Transport: WS  {args.dur}s per FPS level")
    print("=" * 72)

    # pre-test device state
    d0 = parse_diag(get_diag(args.target))
    print(f"Pre:  heap={d0.get('heap','?')} loopLag={d0.get('loopLag','?')}ms fps={d0.get('fps','?')}")

    # Cap at 150fps -- WS path has no rate gate in firmware (unlike UDP ddpMaxFps).
    # 200fps caused TASK_WDT by flooding tcpip_thread faster than main loop can service WDT.
    fps_levels = [30, 60, 90, 120, 150]

    patterns = [
        # (label, gen_fn, uses_prev, note)
        ("chase",    gen_chase,    False, "best-case compression ~97%"),
        ("twinkle",  gen_twinkle,  True,  "mid-range compression ~90%"),
        ("rainbow",  gen_rainbow,  False, "worst-case, raw fallback"),
    ]

    try:
        async with websockets.connect(
                f"ws://{args.target}/ws",
                max_size=65536, ping_interval=None) as ws:

            await ws.send('{"on":true,"bri":200,"live":true}')
            await asyncio.sleep(0.5)

            all_results = {}

            for label, gen_fn, uses_prev, note in patterns:
                print(f"\n{'='*72}")
                print(f"Pattern: {label}  ({note})")

                # raw sweep
                ceil_raw, res_raw = await fps_sweep(
                    ws, n, dest, label, gen_fn, False, fps_levels, args.dur, uses_prev)

                await asyncio.sleep(1.0)

                # compressed sweep
                ceil_comp, res_comp = await fps_sweep(
                    ws, n, dest, label, gen_fn, True, fps_levels, args.dur, uses_prev)

                all_results[label] = (ceil_raw, ceil_comp, res_raw, res_comp)
                await asyncio.sleep(1.0)

            await ws.send('{"live":false}')
            await asyncio.sleep(0.2)

    except Exception as e:
        print(f"\nWS error: {e}"); return 1

    # post-test device state
    d1 = parse_diag(get_diag(args.target))
    print(f"\nPost: heap={d1.get('heap','?')} loopLag={d1.get('loopLag','?')}ms fps={d1.get('fps','?')}")
    drops = d1.get('drops', 0)
    heapGuard = d1.get('heapGuard', 0)
    pkts = d1.get('pkts', 0)
    print(f"      ddp: pkts={pkts} drops={drops} heapGuard={heapGuard}")

    # summary
    print(f"\n{'='*72}")
    print("SUMMARY -- FPS ceiling before overrun")
    print(f"{'Pattern':<12}  {'RAW ceiling':>11}  {'COMP ceiling':>12}  {'Headroom'}")
    print(f"{'-'*12}  {'-'*11}  {'-'*12}  {'-'*20}")
    for label, (ceil_raw, ceil_comp, _, _) in all_results.items():
        if ceil_raw and ceil_comp:
            headroom = f"{ceil_comp/ceil_raw:.1f}x" if ceil_raw else "n/a"
        else:
            headroom = "n/a"
        print(f"{label:<12}  {str(ceil_raw)+' fps':>11}  {str(ceil_comp)+' fps':>12}  {headroom}")
    print("=" * 72)

    return 0

def main():
    ap = argparse.ArgumentParser(description="DDP-over-WebSocket FPS performance test")
    ap.add_argument("--target", default="169.254.7.1")
    ap.add_argument("--segment", type=int, default=2)
    ap.add_argument("--leds", type=int, default=256)
    ap.add_argument("--dur", type=float, default=5, help="seconds per FPS level (default: 5)")
    args = ap.parse_args()
    sys.exit(asyncio.run(main_async(args)))

if __name__ == "__main__":
    main()
