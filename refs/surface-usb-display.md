# Surface as USB Display Panel — ADR (Final)

## Summary

~1200 lines of code total. $4 in cables. Surface boots in 2 seconds.
Linux host sees it as a standard monitor via xrandr.

## Three Components

**Surface (15KB UEFI app)**: GOP framebuffer + USB bulk read + LZ4 decompress + blit. No OS.

**Bridge (50 lines daemon)**: RK3326/RK3566 dual-gadget USB. read() from host endpoint, write() to Surface endpoint. Existing ROCKNIX hardware + usbgadget scripts.

**Host (800 lines DRM driver)**: Kernel module modeled on drivers/gpu/drm/udl/ (DisplayLink). Registers /dev/dri/cardX. xrandr shows USB-1 display. Desktop extends to Surface.

## Surface UEFI App (~350 lines C, ~15KB binary)

UEFI provides everything for free:
- EFI_GRAPHICS_OUTPUT_PROTOCOL: framebuffer pointer (panel already initialized by firmware)
- EFI_USB_IO_PROTOCOL: USB bulk endpoint access (USB host stack already running)

The app does: find USB device by VID:PID, bulk read in a loop, LZ4 decompress, nearest-neighbor 2x upscale, memcpy to GOP framebuffer.

No kernel. No drivers. No filesystem. No network. No input handling. No interrupts. No timers. Just a polling loop reading USB and writing pixels.

Boot: power on, 2s UEFI POST, 0.1s load 15KB .efi, first frame on screen at ~2.3 seconds.

## Bridge (~50 lines on existing hardware)

RK3326/RK3566 handheld running ROCKNIX with dual USB OTG (dwc2 + dwc3):
- Port A (gadget): receives from Linux host
- Port B (gadget): pushes to Surface

The daemon is literally read() from one FunctionFS endpoint, write() to another. Uses existing ROCKNIX usbgadget configfs scripts for USB gadget setup.

## Host DRM Driver (~800 lines kernel module)

Modeled on drivers/gpu/drm/udl/ (USB DisplayLink driver, ~2000 lines). Our version is simpler — no DisplayLink protocol, just raw bulk + LZ4 delta.

Registers: 1 connector, 1 CRTC, 1 plane, 1 encoder. On page-flip: read DRM framebuffer, LZ4 delta compress changed regions, USB bulk write.

User experience: `xrandr --output USB-1 --mode 1368x912 --right-of eDP-1`

## Wire Protocol

12-byte header + pixel payload per USB bulk transfer:

- Bytes 0-3: magic 0x46425553 ("FBUS")
- Bytes 4-7: flags (0x00=raw RGB565, 0x01=LZ4, 0x02=LZ4 delta)
- Bytes 8-9: width (uint16 LE)
- Bytes 10-11: height (uint16 LE)
- Bytes 12+: pixel data (RGB565, raw or LZ4 compressed)

## Bandwidth (USB 2.0 HS both sides, ~40 MB/s)

At 1368x912 RGB565 at 30fps = 75 MB/s raw.

| Content | LZ4 Delta | USB 2.0 % |
|---|---|---|
| WLED effects | ~1-3 MB/s | 3-8% |
| Desktop GUI | ~5-15 MB/s | 13-38% |
| Video playback | ~20-35 MB/s | 50-88% |
| Static image | ~0 | 0% |

## Bill of Materials

| Component | Lines | Size | Cost |
|---|---|---|---|
| Surface UEFI app | ~350 C | 15 KB .efi | $0 (existing) |
| Bridge daemon | ~50 C | trivial | $0 (existing RK3326/3566) |
| Host DRM driver | ~800 C | ~30 KB .ko | $0 |
| USB cables (2x) | - | - | $4 |
| **Total** | **~1200** | **~45 KB** | **$4** |

## Implementation Order

1. UEFI app: build with gnu-efi, test GOP + USB bulk read on Surface
2. Bridge configfs: extend ROCKNIX usbgadget script for dual-gadget bulk mode
3. Host userspace prototype: libusb pixel pusher (test end-to-end before writing kernel driver)
4. Host DRM driver: proper kernel module for xrandr integration
5. LZ4 delta optimization: profile compression ratio vs CPU

## User Experience

1. Plug USB cables: Host → Bridge → Surface
2. Surface boots in 2 seconds (pure UEFI, 15KB app)
3. Linux detects new display output
4. `xrandr` shows USB-1 at 1368x912
5. Desktop extends to Surface panel
6. Done. Surface is a monitor. Forever. No maintenance.
