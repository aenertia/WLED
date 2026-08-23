#!/usr/bin/env python3
"""DDP over UART PPP throughput benchmark  -- raw vs compressed.

Modes:
  ddp_bench.py                              # full benchmark (default)
  ddp_bench.py --diagnostic                 # send marker pixels, hold 15s
  ddp_bench.py --debug                      # interactive: rainbow at 10fps
  ddp_bench.py --ifs                        # IFS fractal live loop (planar-RLE)
  ddp_bench.py --sweep                      # FPS sweep across codec x pattern
  ddp_bench.py --target <ip>                # target device IP
  ddp_bench.py --rgbw                       # 4-channel RGBW mode (SK6812/TM1814)
"""

import argparse, socket, struct, time, sys, math, random, json
import urllib.request

DDP_DEFAULT_PORT = 4048

class TokenBucket:
    """Rate limiter for PPP link pacing. Prevents UART RX overflow by
    throttling send rate to match link capacity."""
    def __init__(self, rate_bytes_per_sec, burst_bytes=4096):
        self.rate = rate_bytes_per_sec
        self.burst = burst_bytes
        self.tokens = burst_bytes
        self.last = time.monotonic()
        self.total_waited = 0.0

    def consume(self, nbytes):
        now = time.monotonic()
        self.tokens = min(self.burst, self.tokens + (now - self.last) * self.rate)
        self.last = now
        if self.tokens >= nbytes:
            self.tokens -= nbytes
            return 0.0
        return (nbytes - self.tokens) / self.rate

    def wait_and_consume(self, nbytes):
        wait = self.consume(nbytes)
        if wait > 0:
            time.sleep(wait)
            self.total_waited += wait
            now = time.monotonic()
            self.tokens = min(self.burst, self.tokens + (now - self.last) * self.rate)
            self.last = now
            self.tokens -= nbytes

# Set by main() for IFS 2D rendering and DDP routing
_bench_width = 40
_bench_height = 80
_ddp_destination = 0xFF
_lossy_depth = False
DDP_VER1 = 0x40; DDP_PUSH = 0x01; DDP_TYPE_COMPRESSED = 0x80; DDP_RGB = 0x0B
DDP_TYPE_RGBW32 = 0x1B  # 00 011 011  -- RGBW, 8 bits per channel, 4 channels
COMP_NONE = 0x00; COMP_DELTA_RLE = 0x10; COMP_RLE = 0x20
COMP_TUPLE_RLE = 0x50; COMP_PLANAR_RLE = 0x60
COMP_PLANAR = COMP_PLANAR_RLE  # alias used by encode_frame / sweep
MAX_PAYLOAD = 1200  # sized for PPP MRU 1500: 1500-IP(20)-UDP(8)-DDP(10)=1462, use 1200 for safety
PLANAR_MAX = 1460   # planar-RLE must fit in one UDP packet

# ---------------------------------------------------------------------------
# Encoding functions
# ---------------------------------------------------------------------------

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

def rle_tuple_encode(src, channels=3):
    out, nt, i = bytearray(), len(src)//channels, 0
    while i < nt:
        cur = src[i*channels:(i+1)*channels]
        run = 1
        while i+run < nt and src[(i+run)*channels:(i+run+1)*channels] == cur and run < 128: run += 1
        if run >= 3:
            out.append(run-1); out.extend(cur); i += run
        else:
            ls, ll = i, 0
            while i < nt and ll < 128:
                t = src[i*channels:(i+1)*channels]
                a = 1
                while i+a < nt and src[(i+a)*channels:(i+a+1)*channels] == t and a < 3: a += 1
                if a >= 3: break
                i += 1; ll += 1
            if ll: out.append(0x80|(ll-1)); out.extend(src[ls*channels:(ls+ll)*channels])
    return bytes(out)

def rle_planar_encode(src, channels=3):
    out = bytearray()
    for ch in range(channels):
        plane = bytes(src[i*channels+ch] for i in range(len(src)//channels))
        enc = rle_encode(plane)
        out.extend(struct.pack('<H', len(enc))); out.extend(enc)
    return bytes(out)

def make_packets(data, seq=1, push=True, comp=COMP_NONE, data_type=DDP_RGB, max_payload=MAX_PAYLOAD, channel_offset=0):
    pkts, off = [], 0
    while off < len(data):
        chunk = min(max_payload, len(data)-off)
        last = off+chunk >= len(data)
        flags = DDP_VER1
        if last and push: flags |= DDP_PUSH
        pkt_data_type = data_type | DDP_TYPE_COMPRESSED if comp != COMP_NONE else data_type
        hdr = struct.pack("!BBBBIH", flags, (seq&0x0F)|(comp&0xF0), pkt_data_type, _ddp_destination, channel_offset+off, chunk)
        pkts.append(hdr + data[off:off+chunk]); off += chunk
        seq = (seq % 15) + 1  # wrap 1-15, never 0
    return pkts, seq

# ---------------------------------------------------------------------------
# Planar-RLE with fallback (from ifs_ppp.py) + sweep codec dispatcher
# ---------------------------------------------------------------------------

# PackBits-style per-channel RLE -- same algorithm as rle_encode
_plane_rle = rle_encode

def planar_rle_encode(rgb_bytes):
    """Deinterleave RGB into 3 planes, RLE each, prefix with 2LE length.
    Returns (encoded, COMP_PLANAR) if result fits one packet, else
    (rgb_bytes, COMP_NONE) fallback."""
    n = len(rgb_bytes) // 3
    r = bytearray(n); g = bytearray(n); b = bytearray(n)
    for i in range(n):
        r[i] = rgb_bytes[i*3]; g[i] = rgb_bytes[i*3+1]; b[i] = rgb_bytes[i*3+2]
    out = bytearray()
    for plane in (r, g, b):
        enc = _plane_rle(plane)
        out += struct.pack('<H', len(enc))
        out += enc
    if len(out) <= PLANAR_MAX:
        return bytes(out), COMP_PLANAR
    return rgb_bytes, COMP_NONE

def encode_frame(codec, cur, prev):
    """Encode a frame with the named codec. Returns (payload, comp_type).
    prev may be None (keyframe)."""
    if codec == 'raw':
        return cur, COMP_NONE
    if codec == 'rle':
        enc = rle_encode(cur)
        return (enc, COMP_RLE) if len(enc) < len(cur) else (cur, COMP_NONE)
    if codec == 'delta-rle':
        if prev and len(prev) == len(cur):
            delta = bytes(a ^ b for a, b in zip(cur, prev))
            drle = rle_encode(delta)
            rle = rle_encode(cur)
            if len(drle) <= len(rle) and len(drle) < len(cur):
                return drle, COMP_DELTA_RLE
            if len(rle) < len(cur):
                return rle, COMP_RLE
        enc = rle_encode(cur)
        return (enc, COMP_RLE) if len(enc) < len(cur) else (cur, COMP_NONE)
    if codec == 'planar-rle':
        return planar_rle_encode(cur)
    return cur, COMP_NONE

def udp_packets(payload, comp, seq, destination=None, max_chunk=MAX_PAYLOAD):
    """Generator yielding (pkt_bytes, new_seq). Advances channelOffset per chunk."""
    if destination is None:
        destination = _ddp_destination
    dtype = DDP_RGB | DDP_TYPE_COMPRESSED if comp != COMP_NONE else DDP_RGB
    off = 0
    while off < len(payload):
        chunk = min(max_chunk, len(payload) - off)
        last = (off + chunk) >= len(payload)
        flags = DDP_VER1 | (DDP_PUSH if last else 0)
        hdr = struct.pack('!BBBBIH',
                          flags, (seq & 0x0F) | (comp & 0xF0),
                          dtype, destination, off, chunk)
        yield hdr + payload[off:off + chunk], (seq % 15) + 1
        off += chunk
        seq = (seq % 15) + 1

# ---------------------------------------------------------------------------
# Lossy colour depth reduction (DedeHai #5810)
# ---------------------------------------------------------------------------

def strip_lsb(color):
    """Adaptive LSB stripping by brightness -- DedeHai #5810."""
    if color > 196: return color & 0xF8
    elif color > 128: return color & 0xFC
    elif color > 64: return color & 0xFE
    return color

def apply_lossy_depth(px, bpp=3):
    d = bytearray(px)
    for i in range(len(d) // bpp):
        d[i*bpp]   = strip_lsb(d[i*bpp])
        d[i*bpp+1] = strip_lsb(d[i*bpp+1])
        d[i*bpp+2] = strip_lsb(d[i*bpp+2])
    return bytes(d)

# ---------------------------------------------------------------------------
# RGB pattern generators (3 bytes/pixel)
# ---------------------------------------------------------------------------

def rainbow(n, t, spd=1.0):
    d = bytearray(n*3)
    for i in range(n):
        h = ((i/n)+t*spd)%1.0*6; c=int(h); f=h-c; q=int(255*(1-f)); tv=int(255*f); x=i*3
        if c==0:   d[x],d[x+1],d[x+2]=255,tv,0
        elif c==1: d[x],d[x+1],d[x+2]=q,255,0
        elif c==2: d[x],d[x+1],d[x+2]=0,255,tv
        elif c==3: d[x],d[x+1],d[x+2]=0,q,255
        elif c==4: d[x],d[x+1],d[x+2]=tv,0,255
        else:      d[x],d[x+1],d[x+2]=255,0,q
    return bytes(d)

def solid_pulse(n, t):
    bri = int(127.5+127.5*math.sin(t*6.28))
    return bytes([bri,0,0]*n)

def sparse_twinkle(n, t, prev=None, density=0.02):
    random.seed(int(t*1000))
    d = bytearray(prev) if prev else bytearray(n*3)
    for _ in range(max(1,int(n*density))):
        px=random.randint(0,n-1); x=px*3
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255); d[x+2]=random.randint(0,255)
    return bytes(d)

# ---------------------------------------------------------------------------
# IFS 2D Fractal Renderer
# ---------------------------------------------------------------------------
# Bounds are (xmin, xmax, ymin, ymax) in IFS coordinate space.

IFS_PRESETS = {
    "fern": {
        "name": "Barnsley Fern",
        "transforms": [
            (0.00,  0.00,  0.00,  0.16, 0.00, 0.00, 0.01),
            (0.85,  0.04, -0.04,  0.85, 0.00, 1.60, 0.85),
            (0.20, -0.26,  0.23,  0.22, 0.00, 1.60, 0.07),
            (-0.15, 0.28,  0.26,  0.24, 0.00, 0.44, 0.07),
        ],
        "bounds": (-2.5, 2.5, 0.0, 10.0),
        "color": (0, 255, 80),
        "iterations": 50000,
    },
    "sierpinski": {
        "name": "Sierpinski Triangle",
        "transforms": [
            (0.5, 0.0, 0.0, 0.5, 0.0,   0.0,   1/3),
            (0.5, 0.0, 0.0, 0.5, 0.5,   0.0,   1/3),
            (0.5, 0.0, 0.0, 0.5, 0.25,  0.433, 1/3),
        ],
        "bounds": (-0.05, 1.05, -0.05, 0.55),
        "color": (255, 100, 0),
        "iterations": 30000,
    },
    "dragon": {
        "name": "Dragon Curve",
        "transforms": [
            (0.824074, 0.281482, -0.212346, 0.864198, -1.882290, -0.110607, 0.787473),
            (0.088272, 0.520988, -0.463889, -0.377778, 0.785360,  8.095795, 0.212527),
        ],
        "bounds": (-4, 12, -2, 10),
        "color": (80, 160, 255),
        "iterations": 40000,
    },
    "flame": {
        "name": "IFS Flame",
        "transforms": [
            (0.61, 0.0,  0.0,  0.61,  0.5,  0.5, 0.40),
            (0.5, -0.5,  0.5,  0.5,  -0.1,  0.1, 0.30),
            (-0.5, 0.5, -0.5, -0.5,   1.1,  0.9, 0.30),
        ],
        "bounds": (-0.5, 1.5, -0.5, 1.5),
        "color": (255, 60, 20),
        "iterations": 40000,
    },
    "tree": {
        "name": "Fractal Tree",
        "transforms": [
            (0.00, 0.00, 0.00, 0.50, 0.00, 0.00, 0.05),
            (0.42, -0.42, 0.42, 0.42, 0.00, 0.20, 0.40),
            (0.42, 0.42, -0.42, 0.42, 0.00, 0.20, 0.40),
            (0.10, 0.00, 0.00, 0.10, 0.00, 0.20, 0.15),
        ],
        "bounds": (-0.6, 1.1, -0.1, 1.0),
        "color": (60, 200, 60),
        "iterations": 40000,
    },
}

def render_ifs_frame(preset_name, width, height, phase):
    """Render an IFS attractor frame as RGB bytes (width*height*3)."""
    cfg = IFS_PRESETS[preset_name]
    transforms = cfg["transforms"]
    xmin, xmax, ymin, ymax = cfg["bounds"]
    base_r, base_g, base_b = cfg["color"]
    fracW, fracH = xmax - xmin, ymax - ymin
    dispAspect = width / height
    fracAspect = fracW / fracH
    if fracAspect > dispAspect:
        scale = width / fracW
        offX, offY = 0.0, (height - fracH * scale) / 2.0
    else:
        scale = height / fracH
        offX, offY = (width - fracW * scale) / 2.0, 0.0

    iters = max(cfg["iterations"], width * height * 8)
    heat = [0] * (width * height)
    x, y = 0.0, 0.0
    cos_p, sin_p = math.cos(phase * 0.3), math.sin(phase * 0.3)

    for _ in range(iters):
        r = random.random()
        cumulative = 0.0
        for a, b, c, d, e, f, p in transforms:
            cumulative += p
            if r <= cumulative:
                x, y = a*x + b*y + e, c*x + d*y + f
                break
        rx = x * cos_p - y * sin_p * 0.1
        sx = int((rx - xmin) * scale + offX)
        sy = int((ymax - y) * scale + offY)
        if 0 <= sx < width and 0 <= sy < height:
            heat[sy * width + sx] = min(heat[sy * width + sx] + 1, 255)

    pixels = bytearray(width * height * 3)
    max_heat = max(max(heat), 1)
    for i in range(width * height):
        h = heat[i]
        if h > 0:
            intensity = min(255, int(math.log1p(h) / math.log1p(max_heat) * 255))
            if intensity < 128:
                t = intensity / 128.0
                pixels[i*3], pixels[i*3+1], pixels[i*3+2] = int(base_r*t), int(base_g*t), int(base_b*t)
            else:
                t = (intensity - 128) / 127.0
                pixels[i*3] = int(base_r + (255 - base_r) * t)
                pixels[i*3+1] = int(base_g + (255 - base_g) * t)
                pixels[i*3+2] = int(base_b + (255 - base_b) * t)
    return bytes(pixels)

# ---------------------------------------------------------------------------
# Ghost Rider particle effect
# ---------------------------------------------------------------------------

_gr_particles = []
_gr_angle = 0.0
_gr_angle_step = 0.04
_gr_hue = 0.0
_gr_src_x = 0.5
_gr_src_y = 0.5
_gr_vx = 0.012
_gr_vy = 0.008

def _gr_reset():
    """Reset ghost_rider particle state (call between sweep cells)."""
    global _gr_particles, _gr_angle, _gr_angle_step, _gr_hue
    global _gr_src_x, _gr_src_y, _gr_vx, _gr_vy
    _gr_particles = []
    _gr_angle = 0.0
    _gr_angle_step = 0.04
    _gr_hue = 0.0
    _gr_src_x = 0.5
    _gr_src_y = 0.5
    _gr_vx = 0.012
    _gr_vy = 0.008

def ghost_rider(n, t, prev=None):
    """Spiraling emitter with fading particle trail -- mimics PS Ghost Rider."""
    global _gr_particles, _gr_angle, _gr_angle_step, _gr_hue
    global _gr_src_x, _gr_src_y, _gr_vx, _gr_vy
    w, h = _bench_width, _bench_height

    _gr_angle += _gr_angle_step
    _gr_angle_step += math.sin(t * 0.7) * 0.002
    _gr_angle_step = max(-0.15, min(0.15, _gr_angle_step))

    speed = 0.018
    _gr_vx = math.cos(_gr_angle) * speed
    _gr_vy = math.sin(_gr_angle) * speed
    _gr_src_x += _gr_vx
    _gr_src_y += _gr_vy

    margin = 0.05
    if _gr_src_x < margin or _gr_src_x > 1.0 - margin:
        _gr_angle = math.pi - _gr_angle + random.uniform(-0.5, 0.5)
        _gr_src_x = max(margin, min(1.0 - margin, _gr_src_x))
    if _gr_src_y < margin or _gr_src_y > 1.0 - margin:
        _gr_angle = -_gr_angle + random.uniform(-0.5, 0.5)
        _gr_src_y = max(margin, min(1.0 - margin, _gr_src_y))

    _gr_hue = (t * 40) % 360
    for _ in range(2):
        _gr_particles.append([_gr_src_x, _gr_src_y, _gr_hue, 255])

    # Decrement TTL then filter -- correct order
    for p in _gr_particles:
        p[3] -= 4
    _gr_particles = [p for p in _gr_particles if p[3] > 0]

    d = bytearray(n * 3)
    for px, py, hue, ttl in _gr_particles:
        ix = int(px * w) % w
        iy = int(py * h) % h
        idx = iy * w + ix
        if idx >= n:
            continue
        hh = (hue / 60.0) % 6
        c = int(hh); f = hh - c
        bri = int(ttl)
        q = int(bri * (1 - f)); tv = int(bri * f)
        if c == 0:   r, g, b = bri, tv, 0
        elif c == 1: r, g, b = q, bri, 0
        elif c == 2: r, g, b = 0, bri, tv
        elif c == 3: r, g, b = 0, q, bri
        elif c == 4: r, g, b = tv, 0, bri
        else:        r, g, b = bri, 0, q
        o = idx * 3
        d[o]   = min(255, d[o]   + r)
        d[o+1] = min(255, d[o+1] + g)
        d[o+2] = min(255, d[o+2] + b)

    hx = int(_gr_src_x * w) % w
    hy = int(_gr_src_y * h) % h
    ho = (hy * w + hx) * 3
    if ho + 2 < len(d):
        d[ho] = d[ho+1] = d[ho+2] = 255

    return bytes(d)

# ---------------------------------------------------------------------------
# Diagnostic and RGBW pattern generators
# ---------------------------------------------------------------------------

def diagnostic_pattern(n, width):
    """Marker pixels at key positions, rest dim blue background."""
    d = bytearray([0, 0, 20] * n)  # dim blue background
    # Pixel 0: bright red
    d[0], d[1], d[2] = 255, 0, 0
    # End of first row: bright green
    if width > 0 and width-1 < n:
        x = (width-1)*3
        d[x], d[x+1], d[x+2] = 0, 255, 0
    # Start of second row: bright yellow
    if width > 0 and width < n:
        x = width*3
        d[x], d[x+1], d[x+2] = 255, 255, 0
    # Center pixel: bright cyan
    mid = n // 2
    if mid < n:
        x = mid*3
        d[x], d[x+1], d[x+2] = 0, 255, 255
    # Last pixel: bright white
    x = (n-1)*3
    d[x], d[x+1], d[x+2] = 255, 255, 255
    # Row markers: first pixel of every 5th row = magenta
    if width > 0:
        for row in range(0, (n + width - 1) // width, 5):
            px = row * width
            if px > 0 and px < n:
                x = px*3
                d[x], d[x+1], d[x+2] = 255, 0, 255
    return bytes(d)

# --- RGBW pattern generators (4 bytes/pixel) ---

def rainbow_rgbw(n, t, spd=1.0):
    """RGB rainbow with W channel as brightness envelope."""
    d = bytearray(n*4)
    for i in range(n):
        h = ((i/n)+t*spd)%1.0*6; c=int(h); f=h-c; q=int(255*(1-f)); tv=int(255*f); x=i*4
        if c==0:   d[x],d[x+1],d[x+2]=255,tv,0
        elif c==1: d[x],d[x+1],d[x+2]=q,255,0
        elif c==2: d[x],d[x+1],d[x+2]=0,255,tv
        elif c==3: d[x],d[x+1],d[x+2]=0,q,255
        elif c==4: d[x],d[x+1],d[x+2]=tv,0,255
        else:      d[x],d[x+1],d[x+2]=255,0,q
        # W channel: sine brightness envelope across the strip
        w = int(127.5 + 127.5 * math.sin((i / n) * math.pi * 2 + t * 4))
        d[x+3] = w
    return bytes(d)

def solid_rgbw(n, r=0, g=0, b=0, w=128):
    return bytes([r, g, b, w] * n)

def solid_pulse_rgbw(n, t):
    """Red pulse with warm white W channel."""
    bri = int(127.5+127.5*math.sin(t*6.28))
    w = int(63.75+63.75*math.sin(t*3.14))  # W at half-speed, half-intensity
    return bytes([bri, 0, 0, w]*n)

def sparse_twinkle_rgbw(n, t, prev=None, density=0.02):
    random.seed(int(t*1000))
    d = bytearray(prev) if prev else bytearray(n*4)
    for _ in range(max(1,int(n*density))):
        px=random.randint(0,n-1); x=px*4
        d[x]=random.randint(0,255); d[x+1]=random.randint(0,255)
        d[x+2]=random.randint(0,255); d[x+3]=random.randint(0,255)
    return bytes(d)

def diagnostic_pattern_rgbw(n, width):
    """Marker pixels at key positions with W channel set, rest dim blue+warm."""
    d = bytearray([0, 0, 20, 10] * n)  # dim blue + low warm white
    # Pixel 0: bright red + W
    d[0], d[1], d[2], d[3] = 255, 0, 0, 64
    # End of first row: bright green + W
    if width > 0 and width-1 < n:
        x = (width-1)*4
        d[x], d[x+1], d[x+2], d[x+3] = 0, 255, 0, 64
    # Start of second row: bright yellow + W
    if width > 0 and width < n:
        x = width*4
        d[x], d[x+1], d[x+2], d[x+3] = 255, 255, 0, 64
    # Center pixel: bright cyan + W
    mid = n // 2
    if mid < n:
        x = mid*4
        d[x], d[x+1], d[x+2], d[x+3] = 0, 255, 255, 128
    # Last pixel: bright white + full W
    x = (n-1)*4
    d[x], d[x+1], d[x+2], d[x+3] = 255, 255, 255, 255
    # Row markers: first pixel of every 5th row = magenta + W
    if width > 0:
        for row in range(0, (n + width - 1) // width, 5):
            px = row * width
            if px > 0 and px < n:
                x = px*4
                d[x], d[x+1], d[x+2], d[x+3] = 255, 0, 255, 96
    return bytes(d)

# ---------------------------------------------------------------------------
# Device communication helpers
# ---------------------------------------------------------------------------

def get_diag(target):
    try: return urllib.request.urlopen(f"http://{target}/diag", timeout=3).read().decode().strip()
    except: return "UNREACHABLE"

def _get_diag_retry(target, retries=3):
    """Fetch /diag with retries until heap= is present."""
    for _ in range(retries):
        try:
            txt = urllib.request.urlopen(
                f"http://{target}/diag", timeout=3).read().decode().strip()
            if 'heap=' in txt:
                return txt
        except:
            pass
        time.sleep(0.5)
    return ""

def parse_diag(txt):
    """Parse /diag key=value tokens into dict. Strips (suffix) and ms unit."""
    vals = {}
    for tok in txt.split():
        if '=' in tok:
            k, _, v = tok.partition('=')
            v = v.split('(')[0].rstrip('ms')
            try:    vals[k] = int(v)
            except: vals[k] = v
    return vals

def _set_live(target, on=True):
    """Enable or disable realtime live mode on the device."""
    body = b'{"on":true,"bri":255,"live":true}' if on else b'{"live":false}'
    try:
        req = urllib.request.Request(f"http://{target}/json/state",
            data=body, headers={"Content-Type": "application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except:
        pass

def _sweep_frame(pat_name, n, t, prev):
    """Generate a frame for sweep/ifs mode pattern names."""
    if pat_name == 'rainbow': return rainbow(n, t, 2.0)
    if pat_name == 'pulse': return solid_pulse(n, t)
    if pat_name == 'twinkle': return sparse_twinkle(n, t, prev)
    if pat_name == 'ghost_rider': return ghost_rider(n, t, prev)
    return rainbow(n, t, 2.0)

def resolve_segment(target, seg_id):
    """Query device for segment pixel range. Returns (start_pixel, num_leds, width, height, bpp)."""
    import json
    try:
        state = json.loads(urllib.request.urlopen(f"http://{target}/json/state", timeout=5).read())
        info = json.loads(urllib.request.urlopen(f"http://{target}/json/info", timeout=5).read())
    except Exception as e:
        print(f"Error querying device: {e}")
        sys.exit(1)
    segs = state.get("seg", [])
    if seg_id >= len(segs):
        print(f"Segment {seg_id} not found (device has {len(segs)} segments)")
        sys.exit(1)
    seg = segs[seg_id]
    matrix = info["leds"].get("matrix", {})
    mat_w = matrix.get("w", 1)
    start_x = seg.get("start", 0)
    stop_x = seg.get("stop", 0)
    start_y = seg.get("startY", 0)
    stop_y = seg.get("stopY", 0)
    seg_w = stop_x - start_x
    seg_h = stop_y - start_y if stop_y > start_y else 1
    pixel_offset = start_y * mat_w + start_x
    num_leds = seg_w * seg_h if seg_h > 1 else seg_w
    print(f"Segment {seg_id}: pixels {pixel_offset}-{pixel_offset+num_leds-1} "
          f"({seg_w}x{seg_h}), matrix width={mat_w}")
    return pixel_offset, num_leds, seg_w, seg_h

def send_frame(sock, target, port, data, seq, compressed=False, prev=None, data_type=DDP_RGB, max_payload=MAX_PAYLOAD, inter_pkt_delay=0, rate_limiter=None, channel_offset=0):
    if compressed:
        cd, ct = compress_adaptive(data, prev)
        pkts, seq = make_packets(cd, seq, comp=ct, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
    else:
        pkts, seq = make_packets(data, seq, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
    for i, p in enumerate(pkts):
        if rate_limiter: rate_limiter.wait_and_consume(len(p))
        sock.sendto(p, (target, port))
        if inter_pkt_delay > 0 and i < len(pkts) - 1: time.sleep(inter_pkt_delay)
    return seq

def _generate_frame(pattern, num_leds, t, prev, rgbw):
    bpp = 4 if rgbw else 3
    if rgbw:
        if pattern == "rainbow": px = rainbow_rgbw(num_leds, t, 2.0)
        elif pattern == "solid_pulse": px = solid_pulse_rgbw(num_leds, t)
        elif pattern == "sparse_twinkle": px = sparse_twinkle_rgbw(num_leds, t, prev)
        else: px = rainbow_rgbw(num_leds, t)
    else:
        if pattern == "rainbow": px = rainbow(num_leds, t, 2.0)
        elif pattern == "solid_pulse": px = solid_pulse(num_leds, t)
        elif pattern == "sparse_twinkle": px = sparse_twinkle(num_leds, t, prev)
        elif pattern == "ghost_rider": px = ghost_rider(num_leds, t, prev)
        elif pattern.startswith("ifs_"): px = render_ifs_frame(pattern[4:], _bench_width, _bench_height, t * 10.0)
        else: px = rainbow(num_leds, t)
    if _lossy_depth:
        px = apply_lossy_depth(px, bpp)
    return px

# ---------------------------------------------------------------------------
# Benchmark phases (default mode)
# ---------------------------------------------------------------------------

def run_phase(sock, target, port, num_leds, fps, dur, pattern, compressed, label, data_type=DDP_RGB, rgbw=False, keyframe_interval=10, max_payload=MAX_PAYLOAD, inter_pkt_delay=0, rate_limiter=None, channel_offset=0, codec=None):
    iv = 1.0/fps; sent=0; raw_b=0; wire_b=0; prev=None; seq=1
    bpp = 4 if rgbw else 3
    start = time.monotonic(); nxt = start
    mode_str = "RGBW" if rgbw else "RGB"
    codec_str = {COMP_TUPLE_RLE: "tuple", COMP_PLANAR_RLE: "planar"}.get(codec, "COMP" if compressed else "RAW")
    print(f"\n--- {label}: {fps}fps {pattern} {codec_str} ({mode_str}) ---")
    while time.monotonic()-start < dur:
        now = time.monotonic()
        if now < nxt: time.sleep(max(0, nxt-now-0.0005)); continue
        t = (now-start)/dur
        px = _generate_frame(pattern, num_leds, t, prev, rgbw)
        raw_b += len(px)
        if codec == COMP_TUPLE_RLE:
            cd = rle_tuple_encode(px, bpp)
            if len(cd) < len(px):
                pkts, seq = make_packets(cd, seq, comp=COMP_TUPLE_RLE, data_type=data_type|DDP_TYPE_COMPRESSED, max_payload=max_payload, channel_offset=channel_offset)
            else:
                pkts, seq = make_packets(px, seq, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset); cd = px
            wire_b += len(cd)
        elif codec == COMP_PLANAR_RLE:
            cd = rle_planar_encode(px, bpp)
            # Planar is whole-frame: plane headers only valid at offset 0.
            # Fall back to adaptive if payload exceeds one packet -- splitting
            # would cause firmware to misparse continuation bytes as headers.
            if len(cd) < len(px) and len(cd) <= max_payload:
                pkts, seq = make_packets(cd, seq, comp=COMP_PLANAR_RLE, data_type=data_type|DDP_TYPE_COMPRESSED, max_payload=max_payload, channel_offset=channel_offset)
            else:
                cd, ct = compress_adaptive(px, prev)
                pkts, seq = make_packets(cd, seq, comp=ct, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
            wire_b += len(cd)
        elif compressed:
            cd, ct = compress_adaptive(px, prev)
            pkts, seq = make_packets(cd, seq, comp=ct, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset); wire_b += len(cd)
        else:
            pkts, seq = make_packets(px, seq, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset); wire_b += len(px)
        prev = px
        if keyframe_interval > 0 and sent % keyframe_interval == 0:
            prev = None
        try:
            for ii, p in enumerate(pkts):
                if rate_limiter: rate_limiter.wait_and_consume(len(p))
                sock.sendto(p, (target, port))
                if inter_pkt_delay > 0 and ii < len(pkts) - 1: time.sleep(inter_pkt_delay)
            sent += 1
        except: pass
        nxt += iv
        if time.monotonic() > nxt+iv: nxt = time.monotonic()
    el = time.monotonic()-start; afps = sent/el if el else 0
    ratio = wire_b/raw_b if raw_b else 1
    sv = f"{(1-ratio)*100:.0f}% saved" if ratio < 1 else "no savings"
    print(f"    {sent} frames, {afps:.1f}fps, wire {wire_b/1024:.1f}KB ({sv}), {wire_b/1024/el:.1f} KB/s")
    return {"label":label,"pattern":pattern,"comp":compressed,"fps":round(afps,1),
            "sent":sent,"ratio":round(ratio,4),"kbs":round(wire_b/1024/el,1),
            "wire_kb":round(wire_b/1024,1),"raw_kb":round(raw_b/1024,1)}

# ---------------------------------------------------------------------------
# Diagnostic and debug modes
# ---------------------------------------------------------------------------

def run_diagnostic(target, port, num_leds, width, rgbw=False, max_payload=MAX_PAYLOAD, inter_pkt_delay=0, channel_offset=0):
    """Send diagnostic marker pattern and hold for observation."""
    data_type = DDP_TYPE_RGBW32 if rgbw else DDP_RGB
    bpp = 4 if rgbw else 3
    mode_str = "RGBW" if rgbw else "RGB"
    print("="*70)
    print(f"DDP Diagnostic ({mode_str})  -- {num_leds} LEDs ({width}x{num_leds//width if width else '?'})")
    print(f"Target: {target}:{port}")
    print("="*70)

    diag = get_diag(target)
    print(f"\nDevice state:\n{diag}\n")

    print("Marker pixels:")
    w_note = " + W=64" if rgbw else ""
    print(f"  px[0]        = RED     (255,0,0{',64' if rgbw else ''})    -- top-left corner")
    if width > 0:
        print(f"  px[{width-1}]       = GREEN   (0,255,0{',64' if rgbw else ''})    -- end of row 0")
        print(f"  px[{width}]       = YELLOW  (255,255,0{',64' if rgbw else ''})  -- start of row 1")
    print(f"  px[{num_leds//2}]      = CYAN    (0,255,255{',128' if rgbw else ''})  -- center")
    print(f"  px[{num_leds-1}]      = WHITE   (255,255,255{',255' if rgbw else ''})  -- last pixel")
    if width > 0:
        print(f"  every 5th row start = MAGENTA (255,0,255{',96' if rgbw else ''})")
    bg = "(0,0,20,10)" if rgbw else "(0,0,20)"
    print(f"  background   = DIM BLUE {bg}")

    # Enable live mode
    try:
        req = urllib.request.Request(f"http://{target}/json/state",
            data=b'{"on":true,"bri":255,"live":true}',
            headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except Exception as e:
        print(f"Warning: couldn't set live mode: {e}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if rgbw:
        px = diagnostic_pattern_rgbw(num_leds, width)
    else:
        px = diagnostic_pattern(num_leds, width)
    seq = 1
    print(f"\nSending diagnostic pattern (hold 15s, resend every 500ms)...")
    start = time.monotonic()
    while time.monotonic() - start < 15:
        pkts, seq = make_packets(px, seq, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
        for p in pkts:
            sock.sendto(p, (target, port))
        time.sleep(0.5)
    sock.close()

    print("\nFetching post-send diagnostics...")
    diag = get_diag(target)
    print(f"\n{diag}")

    # Disable live mode
    try:
        req = urllib.request.Request(f"http://{target}/json/state",
            data=b'{"live":false}',
            headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except: pass

    print("\nDone. Check TFT display for marker positions.")

def run_debug(target, port, num_leds, rgbw=False, max_payload=MAX_PAYLOAD, inter_pkt_delay=0, channel_offset=0, codec=None):
    data_type = DDP_TYPE_RGBW32 if rgbw else DDP_RGB
    bpp = 4 if rgbw else 3
    mode_str = "RGBW" if rgbw else "RGB"
    codec_str = {COMP_TUPLE_RLE: "tuple", COMP_PLANAR_RLE: "planar"}.get(codec, "raw")
    print(f"Debug ({mode_str}/{codec_str}): rainbow at 10fps to {target}:{port}, {num_leds} LEDs. Ctrl+C to stop.")
    try:
        req = urllib.request.Request(f"http://{target}/json/state",
            data=b'{"on":true,"bri":128,"live":true}',
            headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except: pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 1; start = time.monotonic()
    try:
        while True:
            t = (time.monotonic() - start) / 10.0
            px = rainbow_rgbw(num_leds, t, 1.0) if rgbw else rainbow(num_leds, t, 1.0)
            if codec == COMP_TUPLE_RLE:
                cd = rle_tuple_encode(px, bpp)
                if len(cd) < len(px):
                    pkts, seq = make_packets(cd, seq, comp=COMP_TUPLE_RLE, data_type=data_type|DDP_TYPE_COMPRESSED, max_payload=max_payload, channel_offset=channel_offset)
                else:
                    pkts, seq = make_packets(px, seq, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
            elif codec == COMP_PLANAR_RLE:
                cd = rle_planar_encode(px, bpp)
                if len(cd) < len(px) and len(cd) <= max_payload:
                    pkts, seq = make_packets(cd, seq, comp=COMP_PLANAR_RLE, data_type=data_type|DDP_TYPE_COMPRESSED, max_payload=max_payload, channel_offset=channel_offset)
                else:
                    cd2, ct = compress_adaptive(px)
                    pkts, seq = make_packets(cd2, seq, comp=ct, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
            else:
                pkts, seq = make_packets(px, seq, data_type=data_type, max_payload=max_payload, channel_offset=channel_offset)
            for p in pkts:
                sock.sendto(p, (target, port))
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        sock.close()
        try:
            req = urllib.request.Request(f"http://{target}/json/state",
                data=b'{"live":false}',
                headers={"Content-Type":"application/json"}, method="POST")
            urllib.request.urlopen(req, timeout=5)
        except: pass

# ---------------------------------------------------------------------------
# IFS fractal live loop mode (--ifs)
# ---------------------------------------------------------------------------

def run_ifs(target, port, width, height, presets, preset_dur, fps, rate_limiter):
    """IFS fractal live loop with planar-RLE compression."""
    num_leds = width * height
    black = bytes(num_leds * 3)

    _set_live(target, True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

    seq = 1; sent = 0; raw_b = 0; wire_b = 0
    start = time.monotonic(); nxt = start
    interval = 1.0 / fps
    last_preset_idx = -1

    print(f"IFS fractal loop -- {width}x{height}, {fps}fps, "
          f"cycling every {int(preset_dur)}s")
    print(f"Presets: {', '.join(presets)}")
    print(f"Codec: planar-RLE (0x60)")
    print()

    try:
        while True:
            now = time.monotonic()
            elapsed = now - start
            preset_idx = int(elapsed / preset_dur) % len(presets)
            preset_name = presets[preset_idx]
            phase = elapsed * 0.4

            if preset_idx != last_preset_idx:
                # Black keyframe on preset switch -- raw, no compression
                pkts, seq = make_packets(black, seq, comp=COMP_NONE)
                for p in pkts:
                    if rate_limiter:
                        rate_limiter.wait_and_consume(len(p))
                    sock.sendto(p, (target, port))
                time.sleep(0.05)
                last_preset_idx = preset_idx
                print(f"  -> {preset_name}")

            if now < nxt:
                time.sleep(max(0, nxt - now - 0.001))

            px = render_ifs_frame(preset_name, width, height, phase)
            raw_b += len(px)

            enc, comp = planar_rle_encode(px)
            wire_b += len(enc)

            pkts, seq = make_packets(enc, seq, comp=comp)
            for p in pkts:
                if rate_limiter:
                    rate_limiter.wait_and_consume(len(p))
                sock.sendto(p, (target, port))

            sent += 1
            nxt += interval
            if time.monotonic() > nxt + interval:
                nxt = time.monotonic()

            if sent > 0 and sent % 5 == 0:
                el = time.monotonic() - start
                ratio = wire_b / raw_b if raw_b else 1
                print(f"  [{preset_name}] {sent/el:.1f}fps  "
                      f"{wire_b/1024/el:.1f}KB/s  "
                      f"saved {(1-ratio)*100:.0f}%  "
                      f"frame={len(enc)/1024:.1f}KB  ct=0x{comp:02x}")

    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        sock.close()
        _set_live(target, False)

        elapsed = time.monotonic() - start
        if elapsed > 0 and sent > 0:
            ratio = wire_b / raw_b if raw_b else 1
            print(f"\nTotal: {sent} frames in {elapsed:.1f}s = "
                  f"{sent/elapsed:.1f}fps")
            print(f"Wire: {wire_b/1024:.1f} KB "
                  f"({(1-ratio)*100:.0f}% saved)")

# ---------------------------------------------------------------------------
# FPS sweep mode (--sweep)
# ---------------------------------------------------------------------------

def _run_sweep_cell(target, port, num_leds, pat_name, codec, fps_levels,
                    level_dur):
    """Run one codec x pattern cell of the FPS sweep. Returns result dict."""
    rate = TokenBucket(115000, burst_bytes=1210)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

    _set_live(target, True)
    time.sleep(0.3)

    # Baseline drops -- retry until heap= present
    diag_txt = _get_diag_retry(target)
    d0 = parse_diag(diag_txt)
    base_drops = d0.get('drops', 0)

    smooth_fps = 0; max_fps = 0
    smooth_kbs = 0.0; smooth_ratio = 1.0
    peak_lag = 0; total_drops = 0

    for target_fps in fps_levels:
        iv = 1.0 / target_fps
        sent = 0; raw_b = 0; wire_b = 0
        prev = None; seq = 1
        start = time.monotonic(); nxt = start

        while time.monotonic() - start < level_dur:
            now = time.monotonic()
            if now < nxt:
                time.sleep(max(0, nxt - now - 0.001))
                continue
            t = now - start
            cur = _sweep_frame(pat_name, num_leds, t / level_dur, prev)
            raw_b += len(cur)

            # Keyframe every 10 frames
            kf_prev = prev if sent % 10 else None
            payload, comp = encode_frame(codec, cur, kf_prev)
            wire_b += len(payload)

            try:
                for pkt, seq in udp_packets(payload, comp, seq):
                    rate.wait_and_consume(len(pkt))
                    sock.sendto(pkt, (target, port))
                sent += 1
            except OSError:
                break

            prev = cur
            nxt += iv
            if time.monotonic() > nxt + iv:
                nxt = time.monotonic()

        el = time.monotonic() - start
        afps = sent / el if el else 0
        kbs = wire_b / 1024 / el if el else 0
        ratio = wire_b / raw_b if raw_b else 1.0

        time.sleep(1.5)  # settle before polling diag
        diag_txt = _get_diag_retry(target)
        d1 = parse_diag(diag_txt)
        lag = d1.get('loopLag', 0)
        drops_now = d1.get('drops', base_drops)
        delta_drops = max(0, drops_now - base_drops)
        base_drops = drops_now
        total_drops += delta_drops
        peak_lag = max(peak_lag, lag)

        ok = (afps >= target_fps * 0.95
              and delta_drops == 0
              and lag <= 30)
        max_fps = max(max_fps, int(afps))
        if ok:
            smooth_fps = target_fps
            smooth_kbs = kbs
            smooth_ratio = ratio

        print(f"    {target_fps:>4}fps -> {afps:>5.1f}fps  {kbs:>6.1f}KB/s  "
              f"ratio={ratio:.2f}  drops={delta_drops}  lag={lag}ms  "
              f"{'OK' if ok else 'STOP'}")
        if not ok:
            break

    sock.close()
    _set_live(target, False)
    return dict(codec=codec, pattern=pat_name,
                smooth_fps=smooth_fps, max_fps=max_fps,
                kbs=round(smooth_kbs, 1), ratio=round(smooth_ratio, 3),
                drops=total_drops, lag=peak_lag)


def run_sweep(target, port, width, height, codecs, patterns, fps_levels,
              level_dur, out_file):
    """FPS sweep across codec x pattern matrix. UDP/PPP only."""
    num_leds = width * height
    raw_sz = num_leds * 3

    print("=" * 72)
    print("DDP Sweep -- codec x pattern FPS sweep (UDP/PPP)")
    print(f"Target: {target}:{port}, {width}x{height} = {num_leds}px "
          f"({raw_sz}B/frame)")
    print(f"Codecs: {', '.join(codecs)}")
    print(f"Patterns: {', '.join(patterns)}")
    print(f"FPS levels: {fps_levels}")
    print(f"Duration: {level_dur}s per level")
    print("=" * 72)

    diag_txt = _get_diag_retry(target)
    if not diag_txt:
        print("ERROR: device unreachable at", target)
        return
    d = parse_diag(diag_txt)
    print(f"Pre: heap={d.get('heap', '?')}  "
          f"loopLag={d.get('loopLag', '?')}ms  "
          f"ddpSafe={d.get('fps', '?')}fps")
    print()

    results = []

    for codec in codecs:
        for pat_name in patterns:
            _gr_reset()
            print(f"\n[UDP/PPP] codec={codec} pattern={pat_name}")
            r = _run_sweep_cell(target, port, num_leds, pat_name, codec,
                                fps_levels, level_dur)
            results.append(r)
            print(f"  -> smooth={r['smooth_fps']}fps  max={r['max_fps']}fps  "
                  f"{r['kbs']}KB/s  ratio={r['ratio']:.3f}  "
                  f"drops={r['drops']}  lag={r['lag']}ms")
            time.sleep(2)

    # Summary table
    hdr = (f"\n{'Codec':<12} {'Pattern':<12} {'Smooth':>7} {'Max':>5} "
           f"{'KB/s':>7} {'Ratio':>6} {'Drops':>6} {'Lag':>5}")
    sep = "-" * 72
    table_lines = [hdr, sep]
    for r in results:
        row = (f"{r['codec']:<12} {r['pattern']:<12} "
               f"{r['smooth_fps']:>6}fps {r['max_fps']:>4}fps "
               f"{r['kbs']:>7.1f} {r['ratio']:>6.3f} "
               f"{r['drops']:>6} {r['lag']:>4}ms")
        table_lines.append(row)

    table_str = "\n".join(table_lines)
    print("\n" + "=" * 72)
    print("RESULTS")
    print(table_str)
    print("=" * 72)

    diag_txt = _get_diag_retry(target)
    d1 = parse_diag(diag_txt)
    post_str = (f"Post: heap={d1.get('heap', '?')}  "
                f"minheap={d1.get('minheap', '?')}  "
                f"loopLag={d1.get('loopLag', '?')}ms  "
                f"heapGuard={d1.get('heapGuard', '?')}")
    print(f"\n{post_str}")

    with open(out_file, 'w') as f:
        f.write("DDP Sweep Results\n")
        f.write(f"Device: {target}:{port}  {width}x{height}={num_leds}px  "
                f"raw={raw_sz}B/frame\n")
        f.write(f"FPS levels: {fps_levels}  dur={level_dur}s/level\n\n")
        f.write(table_str + "\n")
        f.write(f"\n{post_str}\n")
    print(f"Results saved to {out_file}")

# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="DDP benchmark & diagnostic tool")
    parser.add_argument("--target", default="169.254.7.1", help="Device IP (default: 169.254.7.1)")
    parser.add_argument("--port", type=int, default=DDP_DEFAULT_PORT, help=f"DDP port (default: {DDP_DEFAULT_PORT})")
    parser.add_argument("--width", type=int, default=20, help="Matrix width (default: 20)")
    parser.add_argument("--height", type=int, default=40, help="Matrix height (default: 40)")
    parser.add_argument("--leds", type=int, default=0, help="Total LEDs (overrides width*height)")
    parser.add_argument("--rgbw", action="store_true", help="Use 4-channel RGBW mode (SK6812/TM1814)")
    parser.add_argument("--diagnostic", action="store_true", help="Send marker pattern for position debugging")
    parser.add_argument("--debug", action="store_true", help="Interactive rainbow at 10fps")
    parser.add_argument("--duration", type=int, default=10, help="Phase duration in seconds (default: 10)")
    parser.add_argument("--keyframe-interval", type=int, default=10, help="Force keyframe every N frames (default: 10)")
    parser.add_argument("--baud", type=int, default=1500000, help="Serial baud rate for auto rate-limit (default: 1500000)")
    parser.add_argument("--rate-limit", type=int, default=0, help="Rate limit in KB/s (0=auto from --baud, -1=disabled)")
    parser.add_argument("--mtu", type=int, default=MAX_PAYLOAD, help=f"Max DDP payload bytes per packet (default: {MAX_PAYLOAD})")
    parser.add_argument("--inter-packet-ms", type=float, default=0, help="Delay between packets within a frame (ms). Use 5-30 for PPP links.")
    parser.add_argument("--offset", type=int, default=0, help="Pixel offset for DDP data (default: 0). Converted to channel offset internally.")
    parser.add_argument("--segment", type=int, default=-1, help="Target a specific segment by ID. Queries device for pixel range, overrides --offset/--width/--height/--leds.")
    parser.add_argument("--codec", choices=["adaptive", "tuple", "planar"], default="adaptive", help="Compression codec for --debug and single-pattern runs (default: adaptive)")
    parser.add_argument("--lossy-depth", action="store_true", help="Strip LSBs by brightness before encoding (DedeHai #5810 adaptive colour depth reduction)")

    # IFS fractal live loop
    parser.add_argument("--ifs", action="store_true", help="IFS fractal live loop mode")
    parser.add_argument("--ifs-presets", default="fern,sierpinski,dragon,flame,tree", help="Comma-separated preset names (default: all)")
    parser.add_argument("--ifs-dur", type=float, default=10, help="Seconds per preset (default: 10)")
    parser.add_argument("--ifs-fps", type=int, default=10, help="Target FPS for IFS mode (default: 10)")

    # FPS sweep
    parser.add_argument("--sweep", action="store_true", help="UDP/PPP FPS sweep mode")
    parser.add_argument("--codecs", default="raw,rle,delta-rle,planar-rle", help="Comma-separated codec list (default: all)")
    parser.add_argument("--patterns", default="rainbow,pulse,twinkle,ghost_rider", help="Comma-separated pattern names (default: all)")
    parser.add_argument("--fps-levels", default="10,20,30,40,50,60,80,100,120", help="Comma-separated FPS targets")
    parser.add_argument("--level-dur", type=float, default=8, help="Seconds per FPS level (default: 8)")
    parser.add_argument("--out", default="/tmp/bench_results.txt", help="Results output file (default: /tmp/bench_results.txt)")

    args = parser.parse_args()

    global _bench_width, _bench_height, _lossy_depth
    _lossy_depth = args.lossy_depth
    target = args.target
    port = args.port
    mtu = args.mtu
    inter_pkt_delay = args.inter_packet_ms / 1000.0
    pixel_offset = args.offset

    global _ddp_destination
    if args.segment >= 0:
        pixel_offset, num_leds, seg_w, seg_h = resolve_segment(target, args.segment)
        _bench_width = seg_w
        _bench_height = seg_h
        _ddp_destination = args.segment + 1
        pixel_offset = 0
        print(f"Mode A: DDP destination={_ddp_destination} (segment {args.segment}), offset=0 (segment-relative)")
    else:
        _bench_width = args.width
        _bench_height = args.height
        num_leds = args.leds if args.leds > 0 else args.width * args.height

    bpp = 4 if args.rgbw else 3
    channel_offset = pixel_offset * bpp

    # Rate limiter: auto-compute from baud rate or use explicit value
    rate_limiter = None
    if args.rate_limit == -1:
        pass  # disabled
    elif args.rate_limit > 0:
        rate_limiter = TokenBucket(args.rate_limit * 1024, burst_bytes=max(4096, mtu + 10))
        print(f"Rate limiter: {args.rate_limit} KB/s (explicit)")
    else:
        # Auto: 85% of baud / 10 (8N1 = 10 bits/byte), minus PPP overhead (~15%)
        auto_rate = int((args.baud / 10) * 0.85)
        rate_limiter = TokenBucket(auto_rate, burst_bytes=max(4096, mtu + 10))
        print(f"Rate limiter: {auto_rate // 1024} KB/s (auto from {args.baud} baud)")
    data_type = DDP_TYPE_RGBW32 if args.rgbw else DDP_RGB

    if args.diagnostic:
        run_diagnostic(target, port, num_leds, _bench_width, rgbw=args.rgbw, max_payload=mtu, inter_pkt_delay=inter_pkt_delay, channel_offset=channel_offset)
        return

    codec_map = {"tuple": COMP_TUPLE_RLE, "planar": COMP_PLANAR_RLE}
    codec = codec_map.get(args.codec)

    if args.debug:
        run_debug(target, port, num_leds, rgbw=args.rgbw, max_payload=mtu, inter_pkt_delay=inter_pkt_delay, channel_offset=channel_offset, codec=codec)
        return

    if args.ifs:
        ifs_presets = [p.strip() for p in args.ifs_presets.split(",")]
        ifs_rate = TokenBucket(115000, burst_bytes=1210)
        run_ifs(target, port, _bench_width, _bench_height, ifs_presets,
                args.ifs_dur, args.ifs_fps, ifs_rate)
        return

    if args.sweep:
        sweep_codecs = [c.strip() for c in args.codecs.split(",")]
        sweep_patterns = [p.strip() for p in args.patterns.split(",")]
        sweep_fps = [int(x.strip()) for x in args.fps_levels.split(",")]
        run_sweep(target, port, _bench_width, _bench_height, sweep_codecs,
                  sweep_patterns, sweep_fps, args.level_dur, args.out)
        return

    # --- Default benchmark mode ---
    mode_str = "RGBW" if args.rgbw else "RGB"
    offset_str = f", pixel offset {pixel_offset}" if pixel_offset else ""
    print("="*70)
    print(f"DDP Benchmark ({mode_str})  -- Raw vs Compressed (RLE + Delta+RLE)")
    print(f"Target: {target}:{port}, {num_leds} LEDs ({_bench_width}x{_bench_height}){offset_str}, {bpp} bytes/pixel")
    print("="*70)
    diag = get_diag(target)
    if "UNREACHABLE" in diag: print("Device unreachable"); sys.exit(1)
    print(f"Pre: {diag}")

    try:
        req = urllib.request.Request(f"http://{target}/json/state",
            data=b'{"on":true,"bri":128,"live":true}',
            headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except: pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    results = []; dur = args.duration

    print("\n--- WARM-UP: 10fps 5s ---")
    run_phase(sock, target, port, num_leds, 10, 5, "rainbow", False, "warmup", data_type=data_type, rgbw=args.rgbw, keyframe_interval=args.keyframe_interval, max_payload=mtu, inter_pkt_delay=inter_pkt_delay, rate_limiter=rate_limiter, channel_offset=channel_offset)
    time.sleep(1)

    max_fps = 10 if args.segment >= 0 else 999  # safe limit for per-segment dev testing
    phases = [
        (max_fps, "rainbow",        False, "1-rainbow-raw"),
        (max_fps, "rainbow",        True,  "2-rainbow-comp"),
        (max_fps, "solid_pulse",    False, "3-pulse-raw"),
        (max_fps, "solid_pulse",    True,  "4-pulse-comp"),
        (max_fps, "sparse_twinkle", False, "5-twinkle-raw"),
        (max_fps, "sparse_twinkle", True,  "6-twinkle-comp"),
        (max_fps, "ifs_sierpinski",  False, "7-sierpinski-raw"),
        (max_fps, "ifs_sierpinski",  True,  "8-sierpinski-comp"),
        (max_fps, "ifs_fern",        False, "9-fern-raw"),
        (max_fps, "ifs_fern",        True,  "10-fern-comp"),
        (max_fps, "ifs_dragon",      False, "11-dragon-raw"),
        (max_fps, "ifs_dragon",      True,  "12-dragon-comp"),
        (max_fps, "ifs_flame",       False, "13-flame-raw"),
        (max_fps, "ifs_flame",       True,  "14-flame-comp"),
        (max_fps, "ifs_tree",        False, "15-tree-raw"),
        (max_fps, "ifs_tree",        True,  "16-tree-comp"),
    ]

    for fps, pat, comp, label in phases:
        diag = get_diag(target)
        if "UNREACHABLE" in diag:
            print(f"\n!!! DOWN before {label}"); break
        r = run_phase(sock, target, port, num_leds, fps, dur, pat, comp, label, data_type=data_type, rgbw=args.rgbw, keyframe_interval=args.keyframe_interval, max_payload=mtu, inter_pkt_delay=inter_pkt_delay, rate_limiter=rate_limiter, channel_offset=channel_offset)
        results.append(r); time.sleep(1)
        diag = get_diag(target); r["diag"] = diag
        hp = ""
        for p in diag.split():
            if p.startswith("heap="): hp=p.split("=")[1]
        print(f"    heap={hp}")

    sock.close()
    try:
        req = urllib.request.Request(f"http://{target}/json/state",
            data=b'{"live":false}', headers={"Content-Type":"application/json"}, method="POST")
        urllib.request.urlopen(req, timeout=5)
    except: pass

    print("\n"+"="*70)
    print(f"{'Phase':<14} {'Pattern':<16} {'Mode':<5} {'FPS':>5} {'KB/s':>7} {'Ratio':>7} {'Saved':>7}")
    print("-"*70)
    for r in results:
        m = "COMP" if r["comp"] else "RAW"
        sv = f"{(1-r['ratio'])*100:.0f}%" if r['ratio']<1 else "-"
        print(f"{r['label']:<14} {r['pattern']:<16} {m:<5} {r['fps']:>5.1f} {r['kbs']:>7.1f} {r['ratio']:>6.1%} {sv:>7}")
    print("="*70)

if __name__ == "__main__": main()
