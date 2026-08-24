#!/usr/bin/env python3
"""DDP compression benchmark -- measures 5 codec variants x 6 LED patterns x 2 pixel counts.

Produces a markdown table (stdout) and tools/benchmark_results.csv.
Pure Python computation, no device needed.

Usage:
    python3 tools/ddp_compression_benchmark.py
"""

import csv
import math
import os
import random
import sys
import time

# Add tools/ to path so we can import ddp_codec and ddp_bench
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ddp_codec import (
    rle_encode, rle_decode,
    rle_tuple_encode, rle_tuple_decode,
    rle_planar_encode, rle_planar_decode,
    xor_delta,
)

# ghost_rider uses module-level globals keyed to _bench_width/_bench_height
import ddp_bench

# ---------------------------------------------------------------------------
# Pattern generators (inline -- ddp_bench versions use global state)
# ---------------------------------------------------------------------------

def pat_rainbow(n, frame, total_frames):
    """HSV hue sweep, hue shifts each frame."""
    t = frame / total_frames
    d = bytearray(n * 3)
    for i in range(n):
        h = ((i / n) + t) % 1.0 * 6
        c = int(h); f = h - c
        q = int(255 * (1 - f)); tv = int(255 * f)
        x = i * 3
        if   c == 0: d[x], d[x+1], d[x+2] = 255, tv,  0
        elif c == 1: d[x], d[x+1], d[x+2] = q,   255, 0
        elif c == 2: d[x], d[x+1], d[x+2] = 0,   255, tv
        elif c == 3: d[x], d[x+1], d[x+2] = 0,   q,   255
        elif c == 4: d[x], d[x+1], d[x+2] = tv,  0,   255
        else:        d[x], d[x+1], d[x+2] = 255, 0,   q
    return bytes(d)


def pat_sparse_twinkle(n, frame, total_frames, prev=None, density=0.02):
    """2% random pixels change each frame, rest static."""
    rng = random.Random(frame * 7 + n)
    d = bytearray(prev) if prev else bytearray(n * 3)
    for _ in range(max(1, int(n * density))):
        px = rng.randint(0, n - 1)
        x = px * 3
        d[x]   = rng.randint(0, 255)
        d[x+1] = rng.randint(0, 255)
        d[x+2] = rng.randint(0, 255)
    return bytes(d)


def pat_chase_wipe(n, frame, total_frames, width=4):
    """Bright moving edge (width pixels), rest black."""
    pos = int((frame / total_frames) * n) % n
    d = bytearray(n * 3)
    for i in range(width):
        px = (pos + i) % n
        x = px * 3
        d[x] = d[x+1] = d[x+2] = 255
    return bytes(d)


def pat_gradient_fade(n, frame, total_frames):
    """R ramps 0-255 across strip, brightness pulses each frame."""
    t = frame / total_frames
    bri = 0.3 + 0.7 * (0.5 + 0.5 * math.sin(t * 2 * math.pi))
    d = bytearray(n * 3)
    for i in range(n):
        r = int((i / max(n - 1, 1)) * 255 * bri)
        x = i * 3
        d[x] = min(255, r)
    return bytes(d)


def pat_solid_pulse(n, frame, total_frames):
    """Uniform colour, brightness ramps 0-255-0."""
    t = frame / total_frames
    bri = int(127.5 + 127.5 * math.sin(t * 2 * math.pi))
    return bytes([bri, 0, 0] * n)


def pat_ghost_rider(n, frame, total_frames, prev=None):
    """Delegate to ddp_bench.ghost_rider() with module globals reset per run."""
    t = frame / total_frames
    return ddp_bench.ghost_rider(n, t, prev)


# ---------------------------------------------------------------------------
# Codec wrappers -- each returns (encoded_bytes, decode_func)
# ---------------------------------------------------------------------------

def enc_byte_rle(raw, prev):
    enc = rle_encode(raw)
    return enc, rle_decode

def enc_tuple_rle(raw, prev):
    enc = rle_tuple_encode(raw, 3)
    return enc, lambda d: rle_tuple_decode(d, 3)

def enc_planar_rle(raw, prev):
    enc = rle_planar_encode(raw, 3)
    return enc, lambda d: rle_planar_decode(d, 3)

def enc_delta_byte_rle(raw, prev):
    if prev is None:
        enc = rle_encode(raw)
        return enc, rle_decode
    delta = xor_delta(raw, prev)
    enc = rle_encode(delta)
    def decode(d):
        return rle_decode(d)  # caller must XOR with prev to recover
    return enc, decode

def enc_delta_only(raw, prev):
    if prev is None:
        return raw, lambda d: d
    delta = xor_delta(raw, prev)
    return delta, lambda d: d


VARIANTS = [
    ("byte_rle",       enc_byte_rle),
    ("tuple_rle",      enc_tuple_rle),
    ("planar_rle",     enc_planar_rle),
    ("delta_byte_rle", enc_delta_byte_rle),
    ("delta_only",     enc_delta_only),
]

# ---------------------------------------------------------------------------
# Pattern registry
# ---------------------------------------------------------------------------

# ghost_rider needs state reset between runs
def _reset_ghost_rider(width, height):
    ddp_bench._gr_particles = []
    ddp_bench._gr_angle = 0.0
    ddp_bench._gr_angle_step = 0.04
    ddp_bench._gr_hue = 0.0
    ddp_bench._gr_src_x = 0.5
    ddp_bench._gr_src_y = 0.5
    ddp_bench._gr_vx = 0.012
    ddp_bench._gr_vy = 0.008
    ddp_bench._bench_width = width
    ddp_bench._bench_height = height


PATTERNS = [
    ("rainbow",        False),  # (name, needs_prev_for_generation)
    ("sparse_twinkle", True),
    ("chase_wipe",     False),
    ("gradient_fade",  False),
    ("solid_pulse",    False),
    ("ghost_rider",    True),
]

NUM_FRAMES = 100
PIXEL_COUNTS = [800, 3200]


def generate_frame(pattern_name, n, frame, total_frames, prev_frame):
    if pattern_name == "rainbow":
        return pat_rainbow(n, frame, total_frames)
    elif pattern_name == "sparse_twinkle":
        return pat_sparse_twinkle(n, frame, total_frames, prev_frame)
    elif pattern_name == "chase_wipe":
        return pat_chase_wipe(n, frame, total_frames)
    elif pattern_name == "gradient_fade":
        return pat_gradient_fade(n, frame, total_frames)
    elif pattern_name == "solid_pulse":
        return pat_solid_pulse(n, frame, total_frames)
    elif pattern_name == "ghost_rider":
        return pat_ghost_rider(n, frame, total_frames, prev_frame)
    raise ValueError(f"unknown pattern: {pattern_name}")


def run_benchmark():
    results = []

    for n_pixels in PIXEL_COUNTS:
        raw_size = n_pixels * 3
        # pick a 2D shape for ghost_rider
        if n_pixels <= 800:
            gr_w, gr_h = 20, 40
        else:
            gr_w, gr_h = 40, 80

        for pat_name, pat_needs_prev in PATTERNS:
            # pre-generate all frames for this pattern+size
            if pat_name == "ghost_rider":
                _reset_ghost_rider(gr_w, gr_h)

            frames = []
            prev_gen = None
            for f in range(NUM_FRAMES):
                fr = generate_frame(pat_name, n_pixels, f, NUM_FRAMES, prev_gen)
                frames.append(fr)
                if pat_needs_prev:
                    prev_gen = fr

            for var_name, var_func in VARIANTS:
                ratios = []
                encode_times = []
                decode_times = []
                prev_enc = None

                for f in range(NUM_FRAMES):
                    raw = frames[f]

                    t0 = time.perf_counter()
                    enc, dec_fn = var_func(raw, prev_enc)
                    t1 = time.perf_counter()

                    t2 = time.perf_counter()
                    dec_fn(enc)
                    t3 = time.perf_counter()

                    ratios.append(len(enc) / raw_size)
                    encode_times.append((t1 - t0) * 1e6)
                    decode_times.append((t3 - t2) * 1e6)

                    # delta variants track previous raw frame
                    if var_name.startswith("delta"):
                        prev_enc = raw
                    else:
                        prev_enc = None

                row = {
                    "variant":        var_name,
                    "pattern":        pat_name,
                    "pixels":         n_pixels,
                    "mean_ratio":     sum(ratios) / len(ratios),
                    "mean_encode_us": sum(encode_times) / len(encode_times),
                    "mean_decode_us": sum(decode_times) / len(decode_times),
                    "min_ratio":      min(ratios),
                    "max_ratio":      max(ratios),
                }
                results.append(row)

    return results


def print_markdown(results):
    py_ver = f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    print("## DDP Compression Benchmark Results")
    print()
    print(f"Hardware: Python {py_ver} on host (encode/decode timing only, not ESP32)")
    sizes = sorted(set(r["pixels"] for r in results))
    size_str = ", ".join(f"{s}px ({s*3}B raw)" for s in sizes)
    print(f"Pixel counts: {size_str}")
    print(f"Frames per measurement: {NUM_FRAMES}")
    print()
    print("| Variant | Pattern | Pixels | Mean Ratio | Encode us | Decode us | Min Ratio | Max Ratio |")
    print("|---------|---------|--------|-----------|-----------|-----------|-----------|-----------|")
    for r in results:
        print(f"| {r['variant']:<15} | {r['pattern']:<15} | {r['pixels']:<6} "
              f"| {r['mean_ratio']:<9.3f} | {r['mean_encode_us']:<9.0f} "
              f"| {r['mean_decode_us']:<9.0f} | {r['min_ratio']:<9.3f} "
              f"| {r['max_ratio']:<9.3f} |")


def write_csv(results, path):
    fields = ["variant", "pattern", "pixels", "mean_ratio",
              "mean_encode_us", "mean_decode_us", "min_ratio", "max_ratio"]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in results:
            w.writerow({k: (f"{r[k]:.6f}" if isinstance(r[k], float) else r[k])
                        for k in fields})


def main():
    print("Running benchmark...", file=sys.stderr)
    t0 = time.monotonic()
    results = run_benchmark()
    elapsed = time.monotonic() - t0
    print(f"Done in {elapsed:.1f}s ({len(results)} data points)", file=sys.stderr)

    print_markdown(results)

    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "benchmark_results.csv")
    write_csv(results, csv_path)
    print(f"\nCSV written to {csv_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
