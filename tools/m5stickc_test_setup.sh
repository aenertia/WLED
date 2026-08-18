#!/bin/bash
# M5StickC PPP test setup — canonical segment configuration
# Run after every flash to ensure consistent test state.
#
# Device: M5StickC connected via PPP at 169.254.7.1
# Matrix: 40w x 112h (panel0: TFT 40x80, panel1: strip 8x32 serpentine at y=80)
# Bus0: TFT (type=72, start=0, len=3200, pins=15/13/5/23)
# Bus1: WS2812 (type=22, start=3200, len=256, pin=26)
#
# Canonical test segments:
#   seg0: TFT panel  — cols 0-40, rows 0-80,  ON=false (skip-show gate active)
#   seg1: Strip panel — cols 0-8,  rows 80-112, ON=true, fx=9 (Rainbow)
#
# Usage:
#   ./tools/m5stickc_test_setup.sh          # setup + save to NVS
#   ./tools/m5stickc_test_setup.sh --verify  # verify only, no changes
#   ./tools/m5stickc_test_setup.sh --tft-on  # enable TFT for TFT testing

set -euo pipefail
DEVICE="${M5_PPP_IP:-169.254.7.1}"
TIMEOUT=5

check_device() {
    if ! curl -s --connect-timeout "$TIMEOUT" "http://$DEVICE/json/info" >/dev/null 2>&1; then
        echo "ERROR: Device not reachable at $DEVICE" >&2
        echo "  - Is pppd running? Check: pgrep -a pppd" >&2
        echo "  - Is USB connected? Check: ls /dev/ttyUSB0" >&2
        exit 1
    fi
}

get_info() {
    curl -s --connect-timeout "$TIMEOUT" "http://$DEVICE/json/info" 2>/dev/null | python3 -c "
import sys, json
d = json.load(sys.stdin)
leds = d.get(leds, {})
print(freeheap=%s fps=%s uptime=%s ver=%s totalLeds=%s % (
    d.get(freeheap), leds.get(fps), d.get(uptime), d.get(ver), leds.get(count)))
"
}

get_segments() {
    curl -s --connect-timeout "$TIMEOUT" "http://$DEVICE/json/state" 2>/dev/null | python3 -c "
import sys, json
d = json.load(sys.stdin)
segs = d.get(seg, [])
for i, s in enumerate(segs):
    print(seg%d: on=%s start=%s stop=%s startY=%s stopY=%s fx=%s bri=%s % (
        i, s.get(on), s.get(start), s.get(stop),
        s.get(startY,?), s.get(stopY,?), s.get(fx,?), s.get(bri,?)))
"
}

set_canonical_segments() {
    local tft_on="${1:-false}"
    echo "Setting canonical segments (TFT on=$tft_on)..."
    curl -s -X POST "http://$DEVICE/json/state" \
        -H "Content-Type: application/json" \
        -d "{
            \"on\":true, \"bri\":128,
            \"seg\":[
                {\"id\":0, \"on\":$tft_on, \"start\":0, \"stop\":40, \"startY\":0, \"stopY\":80, \"fx\":0, \"bri\":255, \"col\":[[0,0,0]]},
                {\"id\":1, \"on\":true, \"start\":0, \"stop\":8, \"startY\":80, \"stopY\":112, \"fx\":9, \"sx\":128, \"ix\":128, \"pal\":11, \"bri\":255}
            ]
        }" --connect-timeout "$TIMEOUT" >/dev/null 2>&1
    echo "Saving to NVS..."
    sleep 0.5
    curl -s -X POST "http://$DEVICE/json/state" \
        -H "Content-Type: application/json" \
        -d "{\"psave\":1}" --connect-timeout "$TIMEOUT" >/dev/null 2>&1
    sleep 0.5
}

case "${1:-setup}" in
    --verify)
        echo "=== Device status ==="
        check_device
        get_info
        echo "=== Segments ==="
        get_segments
        ;;
    --tft-on)
        echo "=== Setting up with TFT ON ==="
        check_device
        set_canonical_segments "true"
        echo "=== Result ==="
        get_info
        get_segments
        ;;
    *)
        echo "=== M5StickC canonical test setup ==="
        check_device
        set_canonical_segments "false"
        echo "=== Result ==="
        get_info
        get_segments
        echo ""
        echo "TFT: off (skip-show active)"
        echo "Strip: rainbow effect, ready for DDP bench"
        echo ""
        echo "Next steps:"
        echo "  DDP bench:  python3 /tmp/raw_ddp_bench.py"
        echo "  Soak test:  python3 /tmp/soak_test.py"
        echo "  Enable TFT: $0 --tft-on"
        echo "  Verify:     $0 --verify"
        ;;
esac

# ============================================================
# PlatformIO variant fix (required after framework-arduinoespressif32 update)
# ============================================================
# The m5stick-c board JSON maps to variant "m5stick_c" but the framework
# ships the directory as "m5stack_stickc". Without this symlink, ALL
# compilation fails with "pins_arduino.h: No such file or directory".
#
# Fix:
#   ln -sfn m5stack_stickc ~/.platformio/packages/framework-arduinoespressif32/variants/m5stick_c
#
# This is idempotent — safe to re-run after any PlatformIO update.
