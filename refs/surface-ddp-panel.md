# Surface Tablet as Serial Pixel Display — ADR (Rev 2)

## Concept (Simplified)

The Surface tablet is a **giant addressable NeoPixel matrix** driven over USB serial.
No networking. No OS. A pure UEFI application reads pixel data from USB serial and
writes it to the GOP (Graphics Output Protocol) framebuffer. Boot in 2-3 seconds.

## Architecture

```
WLED (ESP32)                    Surface Tablet
┌──────────────┐   USB Serial  ┌─────────────────────────┐
│ BusSerial     │─────────────→│ UEFI Application        │
│ (Adalight or  │  Adalight/   │  USB Serial → parse     │
│  TPM2 format) │  TPM2 stream │  pixels → upscale       │
│              │              │  → GOP framebuffer       │
│ 342x228      │  ~7MB/s      │  → 2736x1824 panel      │
│ virtual px   │  @30fps      │                          │
└──────────────┘              │  NO OS. NO NETWORK.      │
                               │  Just EFI + USB + GOP.   │
                               └─────────────────────────┘
```

## Why This Is Trivially Simple

WLED already supports serial pixel output:
- `'l'` command: LED data as JSON array (Serial.println)
- `'L'` command: LED data as TPM2 binary packet
- `'O'` command: enable continuous serial streaming
- Adalight protocol: bidirectional (WLED can both receive AND send)

The Surface side needs exactly THREE things:
1. Read bytes from USB serial (UEFI `EFI_SERIAL_IO_PROTOCOL`)
2. Parse the pixel stream (Adalight or TPM2 — trivial)
3. Write pixels to the framebuffer (UEFI `EFI_GRAPHICS_OUTPUT_PROTOCOL`)

That's it. No IP stack, no WiFi, no DHCP, no mDNS, no DNS, no TCP, no UDP.

## UEFI Application Approach (RECOMMENDED)

### What UEFI Provides (already, with zero drivers)

| UEFI Protocol | Purpose | Available on Surface? |
|---|---|---|
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` (GOP) | Framebuffer at native resolution | Yes — this is how you see the Surface logo |
| `EFI_SERIAL_IO_PROTOCOL` | COM port access | Yes (for debug port) |
| `EFI_USB_IO_PROTOCOL` | USB device access | Yes — keyboards/mice work in UEFI |

### GOP Framebuffer

UEFI initializes the panel and provides a linear framebuffer:
```c
EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&gop);

// Framebuffer info
UINT32 width  = gop->Mode->Info->HorizontalResolution;  // 2736
UINT32 height = gop->Mode->Info->VerticalResolution;     // 1824
UINT32 stride = gop->Mode->Info->PixelsPerScanLine;
UINT32 *fb    = (UINT32*)gop->Mode->FrameBufferBase;     // Direct pointer!

// Write a pixel (BGRA32 format)
fb[y * stride + x] = (r << 16) | (g << 8) | b;
```

### USB Serial in UEFI

For USB CDC-ACM (the ESP32's FTDI/CP2102 presents as CDC-ACM to the host):
```c
// Option A: UEFI Serial IO (if USB-serial maps to a COM port)
EFI_SERIAL_IO_PROTOCOL *serial;
serial->Read(serial, &bufsize, buffer);

// Option B: Raw USB IO (direct USB bulk endpoint access)
EFI_USB_IO_PROTOCOL *usb;
usb->UsbBulkTransfer(usb, endpoint, buffer, &len, timeout, &status);
```

**Key question**: Does Surface UEFI enumerate USB CDC-ACM as a serial device?
Most UEFI firmwares do NOT have USB-serial class drivers. We may need to use
raw `EFI_USB_IO_PROTOCOL` to talk to the CDC-ACM bulk endpoints directly.

**Alternative**: Use a USB-to-UART chip that presents as a standard USB serial
device. FTDI FT232 is often supported by UEFI serial IO protocol. The M5StickC
already has an FTDI FT232 — its USB port would appear as serial in UEFI.

### Minimal Linux Fallback

If UEFI USB serial is problematic, fall back to absolute minimal Linux:

```
Kernel: CONFIG_EFI_STUB + CONFIG_FB_EFI + CONFIG_USB_SERIAL_FTDI + CONFIG_USB_ACM
initramfs: busybox + usb-panel binary (reads /dev/ttyUSB0, writes /dev/fb0)
Total: ~3-4MB. Boot: ~5 seconds. No network stack at all.
```

Kernel config — NO NETWORKING:
```
CONFIG_EFI_STUB=y
CONFIG_FB_EFI=y              # efifb framebuffer
CONFIG_USB=y
CONFIG_USB_XHCI_HCD=y
CONFIG_USB_ACM=y             # CDC-ACM serial
CONFIG_USB_SERIAL=y
CONFIG_USB_SERIAL_FTDI=y     # FTDI chips (M5StickC, many ESP32 boards)
CONFIG_USB_SERIAL_CP210X=y   # CP2102 chips (other ESP32 boards)
# CONFIG_NET is not set       ← NO NETWORKING AT ALL
# CONFIG_WIRELESS is not set
# CONFIG_WIFI is not set
```

Boot image: ~3MB kernel + ~500KB initramfs = ~3.5MB total.

## Serial Pixel Protocol

### Option A: Adalight (simplest — WLED already speaks it)

```
Header: 'A' 'd' 'a' <count_hi> <count_lo> <checksum>
Data:   R0 G0 B0 R1 G1 B1 ... Rn Gn Bn
```

WLED can output Adalight via its serial LED streaming mode.
The Surface parses the same format that Adalight/Prismatik/Hyperion use.

### Option B: TPM2 (better framing)

```
Start: 0xC9  Type: 0xDA  Size: <hi> <lo>  Data: RGB...  End: 0x36
```

WLED outputs TPM2 via the `'L'` serial command.

### Option C: Raw RGB stream (fastest, custom)

```
Sync: 0xFF 0x00 0xFF 0x00 (4-byte magic)
Width: <uint16_le>
Height: <uint16_le>
Data: RGB bytes (width * height * 3)
```

Simple custom protocol. No per-pixel overhead.

## Bandwidth

| Scale | Virtual | Pixels | Frame (RGB) | @30fps | USB Serial? |
|---|---|---|---|---|---|
| 16x | 171x114 | 19,494 | 58 KB | 1.7 MB/s | FTDI 1.5M: NO. CP2102 3M: YES |
| **8x** | **342x228** | **77,976** | **234 KB** | **7.0 MB/s** | **Need ≥7Mbps USB serial** |
| 4x | 684x456 | 311,904 | 936 KB | 28 MB/s | Impossible over UART |

**Challenge**: FTDI at 1.5Mbps maxes at ~150KB/s = 5,000 pixels @30fps.
For 8x scale (77,976 pixels) at 30fps we need ~7MB/s — exceeds any UART.

**Solutions**:
1. **16x scale** (171×114 = 19,494 pixels): 1.7MB/s → works at 1.5Mbps FTDI barely.
   Actually no: 1.5Mbps = 150KB/s, need 58KB/frame × 30fps = 1.7MB/s. Still too fast.
2. **Lower framerate**: 19,494 pixels at 10fps = 585KB/s → works at 1.5Mbps? 
   150KB/s / 58KB per frame = 2.5 fps. Too slow.
3. **Pico bridge with USB bulk** (not serial): Pico connects to Surface USB,
   presents as custom USB bulk device, pushes frames at 12Mbps (USB FS).
   12Mbps = 1.5MB/s → 19,494 pixels at 30fps (58KB/frame × 30 = 1.7MB/s) — tight.
4. **ESP32-S3 native USB**: Direct USB bulk at 12Mbps. Same math as Pico.
5. **USB 2.0 High Speed adapter**: If Surface has USB 3.0 ports (all Surface Pro 4+),
   a USB 2.0 HS device runs at 480Mbps → 60MB/s effective. 
   77,976 pixels at 30fps (7MB/s) → trivial.

### The Real Answer: USB Bulk, Not Serial

USB serial (CDC-ACM) is limited by UART baud rates. But **USB bulk transfers**
bypass UART entirely:

| USB Speed | Effective | 8x scale @30fps | Status |
|---|---|---|---|
| USB Full Speed (12Mbps) | ~1 MB/s | 19,494 px (16x scale) | Tight |
| **USB High Speed (480Mbps)** | **~40 MB/s** | **77,976 px (8x scale)** | **Easy** |
| USB 3.0 (5Gbps) | ~400 MB/s | 311,904 px (4x scale) | Overkill |

**ESP32-S3 or Pi Pico**: Both are USB Full Speed (12Mbps).
For 16x scale (171×114) at 30fps → 1.7MB/s → tight but possible.

**For 8x scale (342×228) at 30fps**: Need USB High Speed (480Mbps).
ESP32-P4 has native USB HS. Or use a USB HS hub/adapter with the Pico.

**Practical choice**: 16x scale on ESP32-S3/Pico (USB FS) gives 171×114 = 19,494
virtual "LEDs" on a 2736×1824 panel. Each virtual pixel is a 16×16 block.
Perfectly visible. Good enough for WLED effects visualization.

## WLED Integration — BusSerial

Add a new bus type that outputs pixel data over USB serial/bulk to an external display:

```cpp
class BusSerial : public Bus {
    // Like BusNetwork but over USB serial instead of UDP
    // setPixelColor() → buffer
    // show() → send frame over USB (Adalight or raw format)
};
```

Type IDs 72-79 (same range as BusTFTMatrix — they're both display outputs).

Or simpler: use WLED's existing continuous serial output (`'O'` mode).
Configure the display as a virtual segment, enable serial streaming,
the Surface receives and renders it.

## Comparison: Network DDP vs USB Serial

| Aspect | Network DDP (Rev 1) | USB Serial (Rev 2) |
|---|---|---|
| Transport | WiFi UDP | USB cable |
| Boot complexity | Linux + WiFi + network | EFI app only (no OS) |
| Boot time | ~10 seconds | ~2-3 seconds |
| Image size | ~14MB | ~500KB (EFI) or ~3.5MB (Linux) |
| Bandwidth | 55Mbps (8x@30fps easy) | Limited by USB speed |
| Max resolution | 342x228 @30fps easy | 171x114 @30fps (USB FS) |
| Dependencies | WiFi firmware, wpa_supplicant | Just USB |
| Maintenance | WiFi config, firmware blobs | Zero |
| Cable | None (wireless) | USB cable to WLED controller |

## Recommendation

**For simplest possible implementation**: 
- ESP32-S3 (AtomS3U) plugged into Surface USB
- USB bulk transfer of pixel data (not UART serial)
- 16x scale: 171×114 virtual pixels = 19,494 "LEDs"
- Surface runs pure UEFI application (~500 lines)
- 2-3 second boot, zero OS, zero maintenance

**For higher resolution (8x scale)**:
- Need USB 2.0 HS (480Mbps) — ESP32-P4 or USB HS adapter
- Or WiFi DDP (Rev 1 approach) — more complex but unlimited bandwidth
