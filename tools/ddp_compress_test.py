#!/usr/bin/env python3
"""
DDP compressibility test suite.

Covers the full spectrum from worst-case (incompressible) to best-case
(static/near-zero wire bytes), with both raw and compressed modes for
each pattern so ratios are directly comparable.

Usage:
    # Full strip
    python3 tools/ddp_compress_test.py [--target 169.254.7.1] [--duration 8]

    # Target a specific segment (e.g. WS2812B 8x32 on segment 2)
    python3 tools/ddp_compress_test.py --segment 2 --fps 120

    # With explicit rate limit
    python3 tools/ddp_compress_test.py --segment 2 --fps 120 --rate-limit 150
"""

import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))

import socket, struct, time, math, random, argparse, urllib.request

# -- constants (match ddp_bench.py) --
DDP_VER1 = 0x40; DDP_PUSH = 0x01
DDP_TYPE_COMPRESSED = 0x80; DDP_RGB = 0x0B
COMP_NONE = 0x00; COMP_DELTA_RLE = 0x10; COMP_RLE = 0x20
MAX_PAYLOAD = 1200
TARGET = "169.254.7.1"; PORT = 4048

# -- token bucket rate limiter --
class TokenBucket:
    def __init__(self, rate_bytes_sec, burst_bytes=8192):
        self.rate = rate_bytes_sec
        self.burst = burst_bytes
        self.tokens = burst_bytes
        self.last = time.monotonic()
    def wait_and_consume(self, n):
        now = time.monotonic()
        self.tokens = min(self.burst, self.tokens + (now - self.last) * self.rate)
        self.last = now
        if self.tokens < n:
            time.sleep((n - self.tokens) / self.rate)
            self.tokens = 0
        else:
            self.tokens -= n

# -- codec (identical to ddp_bench.py) --
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

def make_packets(data, seq=1, comp=COMP_NONE, data_type=DDP_RGB, destination=0x01, channel_offset=0):
    pkts, off = [], 0
    while off < len(data):
        chunk = min(MAX_PAYLOAD, len(data)-off)
        last = off+chunk >= len(data)
        flags = DDP_VER1 | (DDP_PUSH if last else 0)
        dt = data_type | DDP_TYPE_COMPRESSED if comp != COMP_NONE else data_type
        hdr = struct.pack("!BBBBIH", flags, (seq&0x0F)|(comp&0xF0), dt, destination, channel_offset+off, chunk)
        pkts.append(hdr + data[off:off+chunk]); off += chunk
        seq = (seq % 15) + 1
    return pkts, seq


def resolve_segment(target, seg_id):
    try:
        state = json.loads(urllib.request.urlopen(f"http://{target}/json/state", timeout=5).read())
        info  = json.loads(urllib.request.urlopen(f"http://{target}/json/info",  timeout=5).read())
    except Exception as e:
        print(f"Error querying device: {e}"); sys.exit(1)
    segs = state.get("seg", [])
    if seg_id >= len(segs):
        print(f"Segment {seg_id} not found (device has {len(segs)})"); sys.exit(1)
    seg   = segs[seg_id]
    mat_w = info["leds"].get("matrix", {}).get("w", 1)
    sx, ex = seg.get("start", 0), seg.get("stop", 0)
    sy, ey = seg.get("startY", 0), seg.get("stopY", 0)
    seg_w  = ex - sx
    seg_h  = ey - sy if ey > sy else 1
    n      = seg_w * seg_h if seg_h > 1 else seg_w
    print(f"Segment {seg_id}: {seg_w}x{seg_h} = {n} LEDs, DDP destination={seg_id+1}")
    return n, seg_id + 1

# -- pattern generators --
def gen_rainbow(n, t):
    """Full-spectrum rainbow cycling -- worst case for delta (every pixel changes)."""
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

def gen_static(n, t):
    """Completely static -- delta is all zeros, best possible compression."""
    return bytes([0x40, 0x80, 0xC0] * n)

def gen_solid_pulse(n, t):
    """Uniform colour, brightness ramps -- all pixels identical, good RLE."""
    bri = int(127.5 + 127.5*math.sin(t*6.28))
    return bytes([bri, 0, int(bri*0.5)] * n)

def gen_solid_colour_change(n, t):
    """Solid colour that snaps to a new hue every second -- keyframe test."""
    hue_idx = int(t * 5) % 6
    colours = [(255,0,0),(0,255,0),(0,0,255),(255,255,0),(0,255,255),(255,0,255)]
    r,g,b = colours[hue_idx]
    return bytes([r,g,b] * n)

def gen_sparse_twinkle_2pct(n, t, prev=None):
    """2% pixel change per frame -- typical LED effect, excellent delta."""
    random.seed(int(t*1000))
    d = bytearray(prev) if prev else bytearray(n*3)
    for _ in range(max(1, int(n*0.02))):
        px = random.randint(0,n-1); x=px*3
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255); d[x+2]=random.randint(0,255)
    return bytes(d)

def gen_sparse_twinkle_10pct(n, t, prev=None):
    """10% pixel change per frame -- moderate change rate."""
    random.seed(int(t*1000)+1)
    d = bytearray(prev) if prev else bytearray(n*3)
    for _ in range(max(1, int(n*0.10))):
        px = random.randint(0,n-1); x=px*3
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255); d[x+2]=random.randint(0,255)
    return bytes(d)

def gen_sparse_twinkle_50pct(n, t, prev=None):
    """50% pixel change per frame -- high change, approaching incompressible."""
    random.seed(int(t*1000)+2)
    d = bytearray(prev) if prev else bytearray(n*3)
    for _ in range(max(1, int(n*0.50))):
        px = random.randint(0,n-1); x=px*3
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255); d[x+2]=random.randint(0,255)
    return bytes(d)

def gen_random_noise(n, t):
    """Pure random noise -- incompressible, tests fallback to raw."""
    random.seed(int(t*10000))
    return bytes(random.randint(0,255) for _ in range(n*3))

def gen_chase(n, t):
    """Moving lit pixel on dark background -- single run of colour, long zero runs."""
    d = bytearray(n*3)
    pos = int(t * n) % n
    width = max(1, n//20)
    for i in range(width):
        x = ((pos+i) % n) * 3
        d[x]=255; d[x+1]=128; d[x+2]=0
    return bytes(d)

def gen_wipe(n, t):
    """Colour wipe -- growing lit region, structured runs."""
    d = bytearray(n*3)
    lit = int(t * n)
    for i in range(min(lit, n)):
        x = i*3; d[x]=0; d[x+1]=200; d[x+2]=255
    return bytes(d)

def gen_gradient_fade(n, t):
    """Smooth spatial gradient that shifts over time -- good for RLE on uniform regions."""
    d = bytearray(n*3)
    for i in range(n):
        v = int(255 * (i/n + t*0.5) % 1.0)
        d[i*3]=v; d[i*3+1]=255-v; d[i*3+2]=int(v*0.5)
    return bytes(d)

def gen_half_half(n, t):
    """First half solid red, second half solid blue -- two long runs."""
    half = n//2
    return bytes([255,0,0]*half + [0,0,255]*(n-half))

def gen_alternating(n, t):
    """Alternating red/blue per pixel -- worst case for byte-level RLE (no runs)."""
    d = bytearray(n*3)
    for i in range(n):
        if i%2==0: d[i*3]=255
        else: d[i*3+2]=255
    return bytes(d)

# -- runner --
PATTERNS = [
    # (name, description, generator_fn, uses_prev)
    ("static",          "Static (no change)",              gen_static,               False),
    ("solid-pulse",     "Solid pulse (uniform, brightness)", gen_solid_pulse,          False),
    ("solid-snap",      "Solid colour snap (keyframe test)", gen_solid_colour_change,  False),
    ("half-half",       "Half red / half blue (2 long runs)", gen_half_half,           False),
    ("chase",           "Chase (1 lit pixel, long zeros)",  gen_chase,                False),
    ("wipe",            "Colour wipe (growing region)",     gen_wipe,                 False),
    ("gradient",        "Gradient fade (smooth spatial)",   gen_gradient_fade,        False),
    ("twinkle-2pct",    "Sparse twinkle 2% change/frame",  gen_sparse_twinkle_2pct,  True),
    ("twinkle-10pct",   "Sparse twinkle 10% change/frame", gen_sparse_twinkle_10pct, True),
    ("twinkle-50pct",   "Sparse twinkle 50% change/frame", gen_sparse_twinkle_50pct, True),
    ("rainbow",         "Rainbow cycle (worst case delta)", gen_rainbow,              False),
    ("alternating",     "Alternating pixels (worst RLE)",   gen_alternating,          False),
    ("random-noise",    "Random noise (incompressible)",    gen_random_noise,         False),
]

def get_diag(target):
    try:
        with urllib.request.urlopen(f"http://{target}/diag", timeout=3) as r:
            return r.read().decode()
    except:
        return "UNREACHABLE"

def run_pattern(sock, target, n, name, desc, gen_fn, uses_prev, dur, compressed,
                fps=120, destination=0x01, rate_limiter=None):
    mode = "COMP" if compressed else "RAW"
    print(f"  {name:<18} {mode:<4}", end="", flush=True)
    iv = 1.0/fps; sent=0; raw_b=0; wire_b=0; prev=None; seq=1
    comp_types = {COMP_NONE:0, COMP_RLE:0, COMP_DELTA_RLE:0}
    start = time.monotonic(); nxt = start
    while time.monotonic()-start < dur:
        now = time.monotonic()
        if now < nxt: time.sleep(max(0, nxt-now-0.0005)); continue
        t = (now-start)/dur
        px = gen_fn(n, t, prev) if uses_prev else gen_fn(n, t)
        raw_b += len(px)
        if compressed:
            cd, ct = compress_adaptive(px, prev)
            comp_types[ct] = comp_types.get(ct,0)+1
            pkts, seq = make_packets(cd, seq, comp=ct, destination=destination)
            wire_b += len(cd)
        else:
            pkts, seq = make_packets(px, seq, destination=destination)
            wire_b += len(px)
        prev = px
        if sent % 10 == 0: prev = None  # keyframe every 10 frames
        try:
            for p in pkts:
                if rate_limiter: rate_limiter.wait_and_consume(len(p))
                sock.sendto(p, (target, PORT))
            sent += 1
        except: pass
        nxt += iv
        if time.monotonic() > nxt+iv: nxt = time.monotonic()
    el = time.monotonic()-start
    afps = sent/el if el else 0
    ratio = wire_b/raw_b if raw_b else 1.0
    saved_pct = (1-ratio)*100
    kbs = wire_b/1024/el if el else 0
    if compressed:
        dominant = max(comp_types, key=comp_types.get)
        ct_str = {COMP_NONE:"raw-fb", COMP_RLE:"RLE", COMP_DELTA_RLE:"dRLE"}[dominant]
    else:
        ct_str = "raw"
    print(f"  {afps:>5.0f}fps  {ratio:>5.1%}  {saved_pct:>+5.0f}%  {kbs:>6.1f}KB/s  [{ct_str}]")
    return {"name":name,"mode":mode,"fps":afps,"ratio":ratio,"saved":saved_pct,"kbs":kbs,"ct":ct_str}

def main():
    ap = argparse.ArgumentParser(description="DDP compressibility test suite")
    ap.add_argument("--target", default=TARGET)
    ap.add_argument("--leds", type=int, default=800)
    ap.add_argument("--segment", type=int, default=-1,
                    help="Target segment ID (queries device for LED count + sets DDP destination byte)")
    ap.add_argument("--fps", type=int, default=120, help="Target FPS (default: 120)")
    ap.add_argument("--rate-limit", type=int, default=0,
                    help="Wire rate limit KB/s (0=none, e.g. 150 for PPP headroom)")
    ap.add_argument("--duration", type=float, default=8, help="seconds per pattern")
    args = ap.parse_args()

    destination = 0x01
    if args.segment >= 0:
        n, destination = resolve_segment(args.target, args.segment)
    else:
        n = args.leds
    raw_frame = n*3

    rate_limiter = None
    if args.rate_limit > 0:
        rate_limiter = TokenBucket(args.rate_limit * 1024, burst_bytes=8192)
        print(f"Rate limiter: {args.rate_limit} KB/s")

    seg_str = f"  segment={args.segment} dest={destination}" if args.segment >= 0 else ""
    print("="*75)
    print(f"DDP Compressibility Test Suite")
    print(f"Target: {args.target}:{PORT}  LEDs: {n}  Raw frame: {raw_frame}B  {args.fps}fps  {args.duration}s/pattern{seg_str}")
    print("="*75)

    diag = get_diag(args.target)
    if "UNREACHABLE" in diag:
        print("ERROR: device unreachable"); return 1
    for line in diag.splitlines():
        if any(k in line for k in ("reset=","heap=","fps=","up=")):
            print(f"  {line.strip()}")
    print()

    # enable live mode
    try:
        req = urllib.request.Request(f"http://{args.target}/json/state",
            data=b'{"on":true,"bri":200,"live":true}',
            headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except: pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

    print(f"  {'Pattern':<18} {'Mode':<4}  {'FPS':>5}    {'Ratio':>5}  {'Saved':>5}  {'Wire':>8}  Codec")
    print(f"  {'-'*18} {'-'*4}  {'-'*5}    {'-'*5}  {'-'*5}  {'-'*8}  {'-'*6}")

    results = []
    for name, desc, gen_fn, uses_prev in PATTERNS:
        r_raw = run_pattern(sock, args.target, n, name, desc, gen_fn, uses_prev,
                            args.duration, compressed=False,
                            fps=args.fps, destination=destination, rate_limiter=rate_limiter)
        time.sleep(0.3)
        r_comp = run_pattern(sock, args.target, n, name, desc, gen_fn, uses_prev,
                             args.duration, compressed=True,
                             fps=args.fps, destination=destination, rate_limiter=rate_limiter)
        results.append((r_raw, r_comp))
        time.sleep(0.5)

    sock.close()

    # disable live mode
    try:
        req = urllib.request.Request(f"http://{args.target}/json/state",
            data=b'{"live":false}', headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except: pass

    # summary table
    print()
    print("="*75)
    print(f"SUMMARY  (raw frame = {raw_frame}B = {raw_frame/1024:.1f}KB)")
    print(f"{'Pattern':<20} {'Raw KB/s':>8}  {'Comp KB/s':>9}  {'Ratio':>6}  {'Saved':>6}  {'Codec':>6}  {'Verdict'}")
    print("-"*75)
    for r_raw, r_comp in results:
        verdict = ""
        if r_comp["saved"] > 80:   verdict = "EXCELLENT"
        elif r_comp["saved"] > 50: verdict = "good"
        elif r_comp["saved"] > 20: verdict = "moderate"
        elif r_comp["saved"] > 0:  verdict = "marginal"
        else:                       verdict = "no gain (raw fallback)"
        print(f"{r_comp['name']:<20} {r_raw['kbs']:>8.1f}  {r_comp['kbs']:>9.1f}  {r_comp['ratio']:>5.1%}  {r_comp['saved']:>+5.0f}%  {r_comp['ct']:>6}  {verdict}")
    print("="*75)

    # final device state
    print()
    diag = get_diag(args.target)
    for line in diag.splitlines():
        if any(k in line for k in ("reset=","heap=","fps=","ddp:","ddpRate")):
            print(f"  {line.strip()}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
