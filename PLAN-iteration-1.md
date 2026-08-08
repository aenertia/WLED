# Iteration Wave 1: Build Environment + Stock WLED on M5StickC

## Overview

Three iteration cycles to get from zero to a verified stock WLED build for M5StickC on koero's NVMe-backed build environment.

All builds happen on **koero** using its NVMe workspace (`/var/mnt/koero/workspace`). NFS is read-only reference.

---

## Iteration 1.1: Build Environment Setup

**Goal**: PlatformIO + ESP-IDF toolchain installed and functional on koero, ready to compile ESP32 targets.

### Decision: Install on koero host, not crossbuild container

**Rationale**: The crossbuild container (Ubuntu 26.04) has Python 3.14 but no pip and no `ensurepip`. Installing pip requires `apt install python3-pip` which pulls in dependencies. The koero host (RHEL 10.2) already has Python 3.12 + pip + the `XDG_CACHE_HOME` → NVMe redirect. PlatformIO is self-contained — it downloads its own ESP-IDF toolchains into `~/.platformio/`. We redirect that to NVMe.

Node.js (needed for WLED web UI build via `tools/cdata.js`) is NOT on the koero host but IS in the crossbuild container. Solution: install Node.js on koero host via `dnf`, or use `npx` — or just run the web asset build step in the crossbuild container and everything else on the host. Simplest: install `nodejs` on koero.

### One-time host zeroconf setup (for wled.local resolution)

```bash
ssh koero
sudo dnf install -y nss-mdns
sudo sed -i 's/^#allow-point-to-point=no/allow-point-to-point=yes/' /etc/avahi/avahi-daemon.conf
sudo systemctl restart avahi-daemon
```

This enables Avahi to discover mDNS services on PPP point-to-point interfaces, and `nss-mdns` makes `.local` names resolvable system-wide (browsers, curl, ping, OpenRGB).

### Tasks

1. **Install PlatformIO CLI on koero host**
   ```bash
   ssh koero
   pip3 install --user platformio
   # Verify
   ~/.local/bin/pio --version
   ```

2. **Redirect PlatformIO storage to NVMe**
   ```bash
   # ~/.platformio would default to /home which is small (269G LV)
   # Redirect to NVMe workspace
   mkdir -p /var/mnt/koero/workspace/buildcache/platformio
   ln -s /var/mnt/koero/workspace/buildcache/platformio ~/.platformio
   ```

3. **Install Node.js on koero host** (for WLED web asset build)
   ```bash
   sudo dnf install -y nodejs npm
   ```

4. **Clone wled repo onto koero NVMe**
   ```bash
   cd /var/mnt/koero/workspace
   git clone ssh://git@git.awa.3d.ae.net.nz:2222/aenertia/wled.git
   cd wled
   git remote add upstream https://github.com/wled/WLED.git
   ```

5. **Verify PlatformIO can resolve the ESP32 platform**
   ```bash
   cd /var/mnt/koero/workspace/wled
   pio platform install espressif32
   # This downloads ~500MB of ESP-IDF + xtensa-gcc toolchain to NVMe
   ```

### Exit Criteria
- [ ] `pio --version` works on koero
- [ ] `~/.platformio` symlinks to NVMe
- [ ] `node --version` works on koero
- [ ] wled repo cloned at `/var/mnt/koero/workspace/wled`
- [ ] `pio platform list` shows `espressif32`

---

## Iteration 1.2: Stock WLED Build for ESP32

**Goal**: Compile stock WLED (no modifications) for a generic ESP32 target. Validates the entire toolchain before touching M5StickC-specific config.

### Tasks

1. **Build the default `esp32dev` environment**
   ```bash
   cd /var/mnt/koero/workspace/wled
   pio run -e esp32dev
   ```
   This is the reference build. If it fails, the toolchain setup is wrong.

2. **Verify build output**
   ```bash
   ls -la .pio/build/esp32dev/firmware.bin
   # Should be ~1.2-1.5 MB
   ```

3. **Review build warnings** — note any ESP-IDF version mismatches or deprecation warnings.

### Exit Criteria
- [ ] `pio run -e esp32dev` completes with exit code 0
- [ ] `firmware.bin` exists and is >1MB
- [ ] No errors (warnings are acceptable)

---

## Iteration 1.3: M5StickC Board Target

**Goal**: Create and validate a `platformio_override.ini` for M5StickC, build successfully.

### Tasks

1. **Create `platformio_override.ini`** with M5StickC environment (stock WLED, WiFi still enabled — just targeting the right board):
   ```ini
   [env:m5stickc]
   extends = esp32_idf_V5
   board = m5stick-c
   platform = ${esp32_idf_V5.platform}
   board_build.partitions = tools/WLED_ESP32_4MB_256KB_FS.csv
   build_flags = ${common.build_flags} ${esp32_idf_V5.build_flags}
     -D WLED_RELEASE_NAME=\"M5StickC\"
     -D DATA_PINS=26
     -D DEFAULT_LED_COUNT=60
     -D BTNPIN=37
     -DARDUINO_USB_CDC_ON_BOOT=0
   lib_deps = ${esp32_idf_V5.lib_deps}
   monitor_speed = 115200
   ```

2. **Build the M5StickC environment**
   ```bash
   pio run -e m5stickc
   ```

3. **Check flash size fit** — M5StickC has 4MB flash. Verify firmware.bin < ~1.5MB (leaves room for LittleFS + OTA partition).

4. **Check board definition** — verify PlatformIO knows `m5stick-c`. If not, check if `esp32dev` with custom flash/partition config works.
   ```bash
   pio boards | grep -i m5stick
   ```

5. **Review**: Compare binary size to the `esp32dev` build. Should be similar or slightly smaller.

### Exit Criteria
- [ ] `pio run -e m5stickc` completes with exit code 0
- [ ] Binary fits in 4MB flash with partitions
- [ ] Board-specific pins (G26 data, G37 button) compiled in

---

## Iteration 1.4: Disable Modules + Memory Baseline

**Goal**: Add the module-disable build flags from the ADR, verify build, measure heap savings.

### Tasks

1. **Update `platformio_override.ini`** to add disable flags:
   ```ini
   [env:m5stickc_lean]
   extends = env:m5stickc
   build_flags = ${env:m5stickc.build_flags}
     -D WLED_DISABLE_ALEXA
     -D WLED_DISABLE_HUESYNC
     -D WLED_DISABLE_INFRARED
     -D WLED_DISABLE_MQTT
     -D WLED_DISABLE_OTA
     -D WLED_DISABLE_ESPNOW
     -D WLED_DISABLE_LOXONE
     -D WLED_DISABLE_2D
     -D WLED_DISABLE_BROWNOUT_DET
   ```
   Note: Keep WiFi, WebSocket, Adalight ENABLED at this stage. WiFi is needed until PPP is implemented.

2. **Build and compare binary sizes**
   ```bash
   pio run -e m5stickc_lean
   ls -la .pio/build/m5stickc/firmware.bin .pio/build/m5stickc_lean/firmware.bin
   ```

3. **If M5StickC is available for flash testing**, flash and check free heap via serial monitor:
   ```bash
   pio run -e m5stickc_lean --target upload --upload-port /dev/ttyUSB0
   pio device monitor -b 115200
   # Look for heap info in WLED startup log
   ```

### Exit Criteria
- [ ] `m5stickc_lean` builds clean
- [ ] Binary size measurably smaller than `m5stickc` (expect ~50-80KB savings)
- [ ] No build errors from disabled modules

---

## Iteration 1.5: PPP Feasibility Spike

**Goal**: Verify ESP-IDF PPP server support compiles with WLED's Arduino framework version. This is a build-only test — no functional PPP yet.

### Tasks

1. **Check ESP-IDF version in WLED's platform config**
   ```bash
   grep -r "platform_packages\|framework" platformio.ini | head -10
   # Need to know: which ESP-IDF version? Does it have CONFIG_LWIP_PPP_SERVER_SUPPORT?
   ```

2. **Create a minimal PPP test** — add `sdkconfig.defaults` or build flags:
   ```ini
   [env:m5stickc_ppp_spike]
   extends = env:m5stickc_lean
   build_flags = ${env:m5stickc_lean.build_flags}
     -D CONFIG_LWIP_PPP_SUPPORT=1
     -D CONFIG_LWIP_PPP_SERVER_SUPPORT=1
   ```

3. **Add a stub file** that `#include`s the PPP headers to verify they're available:
   ```cpp
   // usermods/ppp_spike/ppp_spike.h
   #include "esp_netif.h"
   #include "esp_netif_ppp.h"
   ```

4. **Build** — does it compile? If not, what's missing?

5. **Check sdkconfig approach** — PlatformIO + ESP-IDF may need `sdkconfig.defaults` in the project root rather than `-D` flags for Kconfig options. Test both.

### Exit Criteria
- [ ] PPP headers found and compile
- [ ] `esp_netif_ppp.h` available in the ESP-IDF version WLED uses
- [ ] `CONFIG_LWIP_PPP_SERVER_SUPPORT` accepted (build doesn't error)
- [ ] Documented: which approach works (build flags vs sdkconfig.defaults)

---

## After Wave 1

With these 5 iterations complete, we have:
- Verified build toolchain on koero NVMe
- Stock WLED compiling for M5StickC
- Lean build with disabled modules
- PPP feasibility confirmed at compile level

**Wave 2** starts the actual PPP implementation:
- 2.1: PPP netif creation (UART0, server mode, static IPs)
- 2.2: `WLEDNetworkClass` extension (`isPPP()` following Ethernet pattern)
- 2.3: WiFi bypass when `WLED_USE_PPP` is set
- 2.4: Host-side `pppd` test — web dashboard over serial
- 2.5: DDP pixel streaming over PPP tunnel

**Wave 3** is the M5StickC usermod + polish:
- 3.1: Display usermod (IP, effect, brightness, PPP status)
- 3.2: Button handling
- 3.3: Host auto-connect (udev + systemd)
- 3.4: End-to-end test with physical ARGB hardware

---

## Storage Layout on koero

```
/var/mnt/koero/workspace/
├── wled/                          ← repo clone (NVMe, fast I/O)
│   ├── .pio/                      ← PlatformIO build artifacts
│   ├── platformio_override.ini    ← our board config
│   └── usermods/m5stickc_ppp/     ← our usermod (Wave 2+)
├── buildcache/
│   ├── platformio/ ← ~/.platformio symlink target (ESP-IDF toolchains, ~500MB)
│   ├── npm/                       ← Node.js package cache
│   └── pip/                       ← Python package cache
└── var_tmp/                       ← /var/tmp bind (large temp files)

/tmp/                              ← 16G tmpfs (transient build temps)
```

All build I/O on NVMe (3.4 GB/s seq, 350K IOPS random). Zero NFS dependency during builds.
