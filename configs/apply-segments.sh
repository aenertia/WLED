#!/bin/bash
# Apply 3-segment layout to M5StickC WLED device
#
# Layout:
#   seg0: TFT Main 40x72      (x:0-40, y:0-72)   - any 2D effect (e.g. Plasma fx=97)
#   seg1: TFT Preview 32x8    (x:4-36, y:72-80)  - Copy Segment (fx=77, src=seg2, o1=true, o2=true)
#   seg2: WS2812B 8x32        (x:0-8,  y:80-112) - any 2D effect (e.g. Rainbow fx=9)
#
# seg1 is a rotated preview of seg2 on the bottom strip of the TFT.
# o1=true swaps axes (portrait→landscape), o2=true reads from global frame buffer
# (required when source is on a different bus).
#
# Usage: ./apply-segments.sh [device-ip]
# Default device IP: 169.254.7.1 (PPP link)

DEVICE="${1:-169.254.7.1}"
CONFIG_DIR="$(dirname "$0")"
URL="http://${DEVICE}/json/state"

echo "Applying 3-segment layout to ${DEVICE}..."

curl -s -X POST "${URL}" \
  -H "Content-Type: application/json" \
  -d @"${CONFIG_DIR}/segment-mirror-3seg.json" | python3 -m json.tool 2>/dev/null || echo "(no JSON response)"

echo ""
echo "Applied. Verify at http://${DEVICE}/json/state"
echo ""
echo "Segment layout:"
echo "  seg0: TFT Main 40x72      (x:0-40, y:0-72)   fx=97 (Plasma)"
echo "  seg1: TFT Preview 32x8    (x:4-36, y:72-80)  fx=77 (Copy Segment, src=seg2, o1=true, o2=true)"
echo "  seg2: WS2812B 8x32        (x:0-8,  y:80-112) fx=9  (Rainbow)"
echo ""
echo "To change source segment for preview:"
echo "  curl -X POST http://\${DEVICE}/json/state -d '{\"seg\":{\"id\":1,\"c3\":N}}'"
echo "  (c3 = source segment ID, 0-31)"
echo ""
echo "Key flags for seg1 Copy Segment:"
echo "  o1=true  — swap axes (maps 8x32 portrait → 32x8 landscape)"
echo "  o2=true  — read from global frame buffer (required for cross-bus source)"
