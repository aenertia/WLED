# Surface Tablet as WLED DDP Display Panel — ADR

## Concept

Turn old Surface tablets into "dumb" WLED 2D display panels.
Minimal EFI-stub Linux kernel boots in ~10s, connects to WiFi,
receives DDP pixel data, writes to framebuffer. Zero OS maintenance.

## Architecture

```
WLED Controller ──── WiFi (DDP UDP :4048) ────→ Surface Tablet
  342x228 virtual matrix                         2736x1824 panel
  Any WLED instance                               8x nearest-neighbor upscale
  Effects/presets/DDP                             ~14MB boot image
                                                   No OS, no rootfs, no updates
```

## Surface Display Specs

| Model | Resolution | WiFi | GPU | Linux Status |
|---|---|---|---|---|
| Pro 3 | 2160x1440 | Marvell (quirks) | HD 4400 | Good with patches |
| Pro 4 | 2736x1824 | Marvell (quirks) | HD 515 | Good with patches |
| Pro 5/6 | 2736x1824 | Marvell (quirks) | HD 620 | Good with patches |
| **Pro 7** | **2736x1824** | **Intel (upstream)** | **Iris Plus** | **Best** |
| Pro 8 | 2880x1920 | Intel (upstream) | Iris Xe | Great |
| Go 1 | 1800x1200 | QCA6174 (patch) | HD 615 | Good |
| **Go 2/3** | **1920x1280** | **Intel (upstream)** | **UHD** | **Best small** |

Best targets: Pro 7 or Go 2/3 (Intel WiFi = upstream, no patches).

## Boot Stack (~14MB total)

```
EFI System Partition (FAT32)
  EFI/BOOT/BOOTX64.EFI        ← Linux kernel with CONFIG_EFI_STUB
  initramfs.cpio.gz            ← Everything else:
    /bin/busybox               ← Shell, networking, init (~1MB)
    /bin/wpa_supplicant        ← WiFi auth (~1.5MB static)
    /bin/ddp-receiver          ← DDP→framebuffer bridge (~100KB static)
    /lib/firmware/             ← WiFi firmware blob (~1-2MB)
    /etc/wpa_supplicant.conf   ← WiFi SSID/PSK
    /init                      ← Boot script (mount, wifi, ddp-receiver)
```

Kernel config: EFI_STUB + DRM_SIMPLEDRM (or DRM_I915) + WiFi driver + NET

## DDP Receiver (~200 lines C)

```
UDP :4048 → parse DDP header (10 bytes) → extract pixel offset + RGB data
  → write to virtual buffer (342x228 for SP4 at 8x scale)
  → on PUSH flag: nearest-neighbor upscale → DRM dumb buffer page-flip
```

## Bandwidth Budget

| Scale | Virtual | Pixels | Packets/frame | @30fps | WiFi % |
|---|---|---|---|---|---|
| 16x | 171x114 | 19,494 | 41 | 1.7 MB/s (14Mbps) | ~5% |
| **8x** | **342x228** | **77,976** | **163** | **6.9 MB/s (55Mbps)** | **~18%** |
| 4x | 684x456 | 311,904 | 650 | 27.5 MB/s (220Mbps) | ~73% |

8x scale at 30fps uses 18% of WiFi ac capacity. Comfortable.

## Zero-Maintenance Properties

- No rootfs — everything in initramfs (RAM, read-only)
- No disk writes — SSD never touched after boot
- No updates — frozen firmware image
- No login — init goes straight to ddp-receiver
- Power cycle = clean boot every time
- Total image: ~14MB on EFI System Partition

## Implementation Phases

1. Write ddp-receiver (C, static binary, ~200 LOC)
2. Build minimal linux-surface kernel (EFI_STUB + simpledrm + WiFi)
3. Create initramfs with busybox + wpa_supplicant + ddp-receiver + firmware
4. Test on Surface Pro 7 (Intel WiFi, best Linux support)
5. Create build script that produces the complete EFI image
6. Document per-model WiFi firmware requirements
