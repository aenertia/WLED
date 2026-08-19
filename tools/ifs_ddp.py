#!/usr/bin/env python3
"""IFS fractal renderer  -> DDP over PPP.

Renders Iterated Function System attractors on the host and streams
to ESP32 TFT at native 160x80 resolution via compressed DDP.
Multi-packet DDP support for frames exceeding MAX_PAYLOAD.

Usage:
    python3 ifs_ddp.py                              # Barnsley fern, 160x80
    python3 ifs_ddp.py --preset flame --fps 15
    python3 ifs_ddp.py --width 80 --height 40       # half-res
    python3 ifs_ddp.py --preset sierpinski --compress
    python3 ifs_ddp.py --cycle --duration 120
"""

import argparse, math, os, random, socket, struct, sys, time

# Resolve ddp_codec from the same directory as this script
_script_dir = os.path.dirname(os.path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

from ddp_codec import (make_packets, compress_adaptive, xor_delta,
                       COMP_NONE, COMP_RLE, COMP_DELTA_RLE, MAX_PAYLOAD,
                       detect_mtu, max_payload_for_mtu)

DEFAULT_HOST = "169.254.7.1"
DEFAULT_PORT = 4048
DEFAULT_WIDTH = 160
DEFAULT_HEIGHT = 80

# ── IFS Presets ──────────────────────────────────────────────────────
# Each transform: (a, b, c, d, e, f, probability)
# New point: x' = a*x + b*y + e,  y' = c*x + d*y + f

PRESETS = {
    "fern": {
        "name": "Barnsley Fern",
        "transforms": [
            (0.00,  0.00,  0.00,  0.16, 0.00, 0.00, 0.01),   # stem
            (0.85,  0.04, -0.04,  0.85, 0.00, 1.60, 0.85),   # main leaflet
            (0.20, -0.26,  0.23,  0.22, 0.00, 1.60, 0.07),   # left leaflet
            (-0.15, 0.28,  0.26,  0.24, 0.00, 0.44, 0.07),   # right leaflet
        ],
        "bounds": (-2.5, 10.5, -0.5, 2.8),  # ymin, ymax, xmin, xmax
        "color": (0, 255, 80),  # green
        "bg": (0, 0, 0),
        "iterations": 50000,
    },
    "sierpinski": {
        "name": "Sierpinski Triangle",
        "transforms": [
            (0.5, 0.0, 0.0, 0.5, 0.0,   0.0,   1/3),
            (0.5, 0.0, 0.0, 0.5, 0.5,   0.0,   1/3),
            (0.5, 0.0, 0.0, 0.5, 0.25,  0.433, 1/3),
        ],
        "bounds": (-0.05, 0.95, -0.05, 0.55),
        "color": (255, 100, 0),  # orange
        "bg": (0, 0, 8),
        "iterations": 30000,
    },
    "dragon": {
        "name": "Dragon Curve",
        "transforms": [
            (0.824074, 0.281482, -0.212346, 0.864198, -1.882290, -0.110607, 0.787473),
            (0.088272, 0.520988, -0.463889, -0.377778, 0.785360,  8.095795, 0.212527),
        ],
        "bounds": (-4, 12, -2, 10),
        "color": (80, 160, 255),  # cyan-blue
        "bg": (0, 0, 0),
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
        "color": (255, 60, 20),  # flame orange-red
        "bg": (0, 0, 0),
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
        "bounds": (-0.6, 1.1, -1.0, 1.0),
        "color": (60, 200, 60),
        "bg": (4, 2, 0),
        "iterations": 40000,
    },
}


def render_ifs(preset, width, height, phase=0.0, iterations=0):
    """Render IFS attractor into a pixel buffer with animation phase."""
    cfg = PRESETS[preset]
    transforms = cfg["transforms"]
    ymin, ymax, xmin, xmax = cfg["bounds"]
    base_r, base_g, base_b = cfg["color"]
    bg_r, bg_g, bg_b = cfg["bg"]

    fracW = xmax - xmin
    fracH = ymax - ymin
    dispAspect = width / height
    fracAspect = fracW / fracH
    if fracAspect > dispAspect:
        scale = width / fracW
        offX = 0.0
        offY = (height - fracH * scale) / 2.0
    else:
        scale = height / fracH
        offX = (width - fracW * scale) / 2.0
        offY = 0.0

    iters = iterations if iterations > 0 else max(cfg["iterations"], width * height * 8)

    heat = [0] * (width * height)

    x, y = 0.0, 0.0
    cos_p = math.cos(phase * 0.3)
    sin_p = math.sin(phase * 0.3)

    for _ in range(iters):
        r = random.random()
        cumulative = 0.0
        for a, b, c, d, e, f, p in transforms:
            cumulative += p
            if r <= cumulative:
                nx = a * x + b * y + e
                ny = c * x + d * y + f
                x, y = nx, ny
                break

        rx = x * cos_p - y * sin_p * 0.1
        ry = y

        sx = int((rx - xmin) * scale + offX)
        sy = int((ymax - ry) * scale + offY)

        if 0 <= sx < width and 0 <= sy < height:
            idx = sy * width + sx
            heat[idx] = min(heat[idx] + 1, 255)

    pixels = bytearray(width * height * 3)
    max_heat = max(max(heat), 1)

    for i in range(width * height):
        h = heat[i]
        if h == 0:
            pixels[i*3] = bg_r
            pixels[i*3+1] = bg_g
            pixels[i*3+2] = bg_b
        else:
            # Log-scale intensity for better contrast
            intensity = min(255, int(math.log1p(h) / math.log1p(max_heat) * 255))
            # Color gradient: dim -> base color -> white at peak
            if intensity < 128:
                t = intensity / 128.0
                pixels[i*3]   = int(base_r * t)
                pixels[i*3+1] = int(base_g * t)
                pixels[i*3+2] = int(base_b * t)
            else:
                t = (intensity - 128) / 127.0
                pixels[i*3]   = int(base_r + (255 - base_r) * t)
                pixels[i*3+1] = int(base_g + (255 - base_g) * t)
                pixels[i*3+2] = int(base_b + (255 - base_b) * t)

    return bytes(pixels)


def main():
    parser = argparse.ArgumentParser(
        description="IFS fractal renderer -> DDP over PPP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help="DDP target host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help="DDP target port")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH,
                        help="Frame width in pixels")
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT,
                        help="Frame height in pixels")
    parser.add_argument("--fps", type=int, default=5,
                        help="Target frames per second")
    parser.add_argument("--duration", type=int, default=30,
                        help="Duration in seconds (0 = infinite)")
    parser.add_argument("--iterations", type=int, default=0,
                        help="IFS iterations per frame (0=auto from preset)")
    parser.add_argument("--preset", default="fern", choices=sorted(PRESETS.keys()),
                        help="IFS preset to render")
    parser.add_argument("--compress", action="store_true",
                        help="Enable adaptive compression (delta-RLE)")
    parser.add_argument("--cycle", action="store_true",
                        help="Cycle through all presets")
    parser.add_argument("--keyframe-interval", type=int, default=30,
                        help="Send full keyframe every N frames (0=never)")
    parser.add_argument("--mtu", type=int, default=0,
                        help="Max DDP payload bytes per packet (0=auto-detect from ppp0)")
    args = parser.parse_args()

    w, h = args.width, args.height
    if args.mtu <= 0:
        detected = detect_mtu("ppp0")
        if detected:
            mtu = max_payload_for_mtu(detected)
            print(f"Auto-detected ppp0 MTU={detected} -> DDP payload={mtu}")
        else:
            mtu = MAX_PAYLOAD
            print(f"No ppp0 detected, using default payload={mtu}")
    else:
        mtu = args.mtu

    num_pixels = w * h
    raw_frame_bytes = num_pixels * 3
    packets_per_frame = math.ceil(raw_frame_bytes / mtu)

    # Bandwidth preflight  -- estimate if fps is achievable
    est_compressed = raw_frame_bytes * 0.25 if args.compress else raw_frame_bytes
    est_wire_per_sec = est_compressed * args.fps * 1.08  # ~8% PPP overhead
    est_baud_needed = est_wire_per_sec * 10  # 8N1 = 10 bits/byte
    print(f"IFS Fractal -> DDP | {w}x{h} = {num_pixels} pixels ({raw_frame_bytes} bytes/frame)")
    print(f"Multi-packet: {packets_per_frame} packets/frame (payload={mtu})")
    print(f"Preset: {PRESETS[args.preset]['name'] if not args.cycle else 'ALL (cycling)'}")
    print(f"Target: {args.host}:{args.port} | {args.fps}fps | "
          f"{'compressed' if args.compress else 'raw'}")
    print(f"Est. bandwidth: {est_wire_per_sec/1024:.0f} KB/s "
          f"(needs ~{est_baud_needed/1e6:.1f} Mbaud)")
    print()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 1
    prev = None
    sent = 0
    wire_total = 0
    raw_total = 0

    presets = list(PRESETS.keys()) if args.cycle else [args.preset]
    preset_idx = 0

    start = time.monotonic()
    try:
        while True:
            elapsed_total = time.monotonic() - start
            if args.duration > 0 and elapsed_total >= args.duration:
                break

            frame_start = time.monotonic()
            phase = elapsed_total * 0.5
            current_preset = presets[preset_idx % len(presets)]

            pixels = render_ifs(current_preset, w, h, phase, iterations=args.iterations)
            raw_total += len(pixels)

            # Send via DDP (per-packet compression in make_packets)
            if args.compress:
                kf_interval = args.keyframe_interval
                force_keyframe = kf_interval > 0 and sent % kf_interval == 0
                if prev is not None and len(prev) == len(pixels) and not force_keyframe:
                    send_data = xor_delta(pixels, prev)
                    comp_type = COMP_DELTA_RLE
                else:
                    send_data = pixels
                    comp_type = COMP_RLE
                pkts, seq = make_packets(send_data, seq, comp=comp_type, max_payload=mtu)
                wire_total += sum(len(p) - 10 for p in pkts)
            else:
                pkts, seq = make_packets(pixels, seq, max_payload=mtu)
                wire_total += len(pixels)

            for p in pkts:
                sock.sendto(p, (args.host, args.port))
                time.sleep(0.0005)  # 0.5ms inter-packet gap

            prev = pixels
            sent += 1

            # Cycle presets every 5 seconds
            if args.cycle and sent % max(1, args.fps * 5) == 0:
                black = bytes(w * h * 3)
                pkts, seq = make_packets(black, seq, comp=COMP_RLE, max_payload=mtu)
                for p in pkts:
                    sock.sendto(p, (args.host, args.port))
                preset_idx += 1
                prev = None
                print(f"  -> {PRESETS[presets[preset_idx % len(presets)]]['name']}")

            elapsed = time.monotonic() - frame_start
            sleep_time = (1.0 / args.fps) - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

            if sent % args.fps == 0:
                fps_actual = sent / (time.monotonic() - start)
                ratio = wire_total / raw_total if raw_total else 1
                saved = (1 - ratio) * 100
                print(f"  {sent} frames | {fps_actual:.1f}fps | "
                      f"{wire_total/1024/(time.monotonic()-start):.1f} KB/s | "
                      f"{saved:.0f}% saved")

    except KeyboardInterrupt:
        print("\nStopped.")

    elapsed = time.monotonic() - start
    if elapsed > 0 and sent > 0:
        ratio = wire_total / raw_total if raw_total else 1
        print(f"\nTotal: {sent} frames in {elapsed:.1f}s = {sent/elapsed:.1f}fps")
        print(f"Wire: {wire_total/1024:.1f} KB "
              f"({(1-ratio)*100:.0f}% saved vs {raw_total/1024:.1f} KB raw)")
        print(f"Bandwidth: {wire_total/1024/elapsed:.1f} KB/s")

    sock.close()


if __name__ == "__main__":
    main()
