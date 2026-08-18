#!/bin/bash
# Apply 3-segment mirror layout to M5StickC WLED device
# seg0: TFT 8x32 preview (top-left corner)
# seg1: TFT 40x48 remainder (below preview)
# seg2: WS2812B 8x32 running "Copy Segment" effect mirroring seg0
#
# Usage: ./apply-segments.sh [device-ip]
# Default device IP: 169.254.7.1 (PPP link)

DEVICE="${1:-169.254.7.1}"
CONFIG_DIR="$(dirname "$0")"
URL="http://${DEVICE}/json/state"

echo "Applying 3-segment mirror layout to ${DEVICE}..."

# First, delete extra segments (clear old 9-segment config)
# Then apply the new 3-segment layout
curl -s -X POST "${URL}" \
  -H "Content-Type: application/json" \
  -d @"${CONFIG_DIR}/segment-mirror-3seg.json" | python3 -m json.tool 2>/dev/null || echo "(no JSON response)"

echo ""
echo "Applied. Verify at http://${DEVICE}/json/state"
echo ""
echo "Segment layout:"
echo "  seg0: TFT Preview 8x32    (x:0-8, y:0-32)   - any 2D effect"
echo "  seg1: TFT Main 40x48      (x:0-40, y:32-80)  - any 2D effect"
echo "  seg2: WS2812B Mirror 8x32 (x:0-8, y:80-112)  - Copy Segment (src=seg0)"
echo ""
echo "To change the source segment for seg2, POST:"
echo "  curl -X POST http://${DEVICE}/json/state -d '{\"seg\":{\"id\":2,\"c3\":0}}'"
echo "  (c3 = source segment ID, 0-31)"
