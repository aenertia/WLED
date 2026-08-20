#!/usr/bin/env python3
"""
DDP-over-WebSocket test -- raw and compressed, targeting a specific segment.

Mirrors sendDDP() / sendDDPCompressed() from common.js:
  pkt[0]    = 0x02  (WLED WS binary protocol indicator, P_DDP)
  pkt[1..10] = DDP header (flags, seq|comp_type, dataType, dest, offset[4], len[2])
  pkt[11..] = payload

Usage:
    python3 tools/ddp_ws_test.py [--target 169.254.7.1] [--segment 2] [--fps 120]
"""

import sys, struct, time, math, random, argparse, asyncio
import websockets

DDP_VER1 = 0x40; DDP_PUSH = 0x01
DDP_TYPE_COMPRESSED = 0x80; DDP_RGB = 0x0B
COMP_NONE = 0x00; COMP_DELTA_RLE = 0x10; COMP_RLE = 0x20
WS_DDP = 0x02  # WLED binary protocol byte for DDP-over-WS

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

def gen_rainbow(n, t):
    d = bytearray(n*3)
    for i in range(n):
        h = ((i/n)+t*2.0)%1.0*6; c=int(h); f=h-c; q=int(255*(1-f)); tv=int(255*f); x=i*3
        if c==0:   d[x],d[x+1],d[x+2]=255,tv,0
        elif c==1: d[x],d[x+1],d[x+2]=q,255,0
        elif c==2: d[x],d[x+1],d[x+2]=0,255,tv
        elif c==3: d[x],d[x+1],d[x+2]=0,q,255
        elif c==4: d[x],d[x+1],d[x+2]=tv,0,255
        else:      d[x],d[x+1],d[x+2]=255,0,q
    return bytes(d)

def gen_chase(n, t):
    d = bytearray(n*3)
    pos = int(t * n) % n
    for i in range(max(1, n//16)):
        x = ((pos+i)%n)*3; d[x]=0; d[x+1]=200; d[x+2]=255
    return bytes(d)

def gen_twinkle(n, t, prev=None):
    random.seed(int(t*1000))
    d = bytearray(prev) if prev else bytearray(n*3)
    for _ in range(max(1, int(n*0.05))):
        px=random.randint(0,n-1); x=px*3
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255); d[x+2]=random.randint(0,255)
    return bytes(d)

async def run_phase(ws, n, dest, label, gen_fn, compressed, fps, dur, uses_prev=False):
    iv = 1.0/fps; sent=0; raw_b=0; wire_b=0; prev=None; seq=1
    start = time.monotonic(); nxt = start
    while time.monotonic()-start < dur:
        now = time.monotonic()
        if now < nxt:
            await asyncio.sleep(max(0, nxt-now-0.0005))
            continue
        t = (now-start)/dur
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
        if sent % 10 == 0: prev = None  # keyframe every 10 frames
        try:
            await ws.send(pkt)
            sent += 1
        except Exception as e:
            print(f"\n  WS send error: {e}"); break
        seq = (seq % 15) + 1
        nxt += iv
        if time.monotonic() > nxt+iv: nxt = time.monotonic()
    el = time.monotonic()-start
    afps = sent/el if el else 0
    ratio = wire_b/raw_b if raw_b else 1.0
    mode = "COMP" if compressed else "RAW "
    saved = f"+{round((1-ratio)*100)}% saved" if ratio < 1 else "no gain      "
    print(f"  {label:<24} {mode}  {afps:>5.0f}fps  {ratio:>5.1%}  {saved:<14}  {wire_b/1024/el:.1f} KB/s")
    return ratio

async def main_async(args):
    dest = args.segment + 1
    n = args.leds

    print("="*72)
    print("DDP-over-WebSocket test")
    print(f"Target: ws://{args.target}/ws  segment={args.segment} dest={dest}  {n} LEDs  {args.fps}fps")
    print(f"Frame:  [0x02][DDP 10B hdr][payload]  -- mirrors common.js sendDDP()")
    print("="*72)

    try:
        async with websockets.connect(f"ws://{args.target}/ws",
                                      max_size=65536, ping_interval=None) as ws:
            print(f"Connected.")

            # enable live mode
            await ws.send('{"on":true,"bri":200,"live":true}')
            await asyncio.sleep(0.5)

            print(f"\n  {'Phase':<24} {'Mode':<4}  {'FPS':>5}    {'Ratio':>5}  {'Savings':<14}  Wire KB/s")
            print(f"  {'-'*24} {'-'*4}  {'-'*5}    {'-'*5}  {'-'*14}  {'-'*9}")

            phases = [
                ("rainbow raw",            gen_rainbow,  False, False),
                ("rainbow compressed",     gen_rainbow,  True,  False),
                ("chase raw",              gen_chase,    False, False),
                ("chase compressed",       gen_chase,    True,  False),
                ("twinkle 5% raw",         gen_twinkle,  False, True),
                ("twinkle 5% compressed",  gen_twinkle,  True,  True),
            ]

            for label, gen_fn, compressed, uses_prev in phases:
                await run_phase(ws, n, dest, label, gen_fn, compressed,
                                args.fps, args.duration, uses_prev)
                await asyncio.sleep(0.3)

            await ws.send('{"live":false}')
            await asyncio.sleep(0.2)

        print("\nWS connection closed cleanly.")
    except Exception as e:
        print(f"Error: {e}"); return 1
    return 0

def main():
    ap = argparse.ArgumentParser(description="DDP-over-WebSocket test")
    ap.add_argument("--target", default="169.254.7.1")
    ap.add_argument("--segment", type=int, default=2)
    ap.add_argument("--leds", type=int, default=256)
    ap.add_argument("--fps", type=int, default=120)
    ap.add_argument("--duration", type=float, default=6)
    args = ap.parse_args()
    sys.exit(asyncio.run(main_async(args)))

if __name__ == "__main__":
    main()
