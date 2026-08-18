# Pi Pico NCM-PPP Bridge

Firmware for Raspberry Pi Pico that bridges USB CDC-NCM Ethernet to UART PPP,
providing zero-config USB networking from a host computer to an ESP32 running WLED.

**Plug in USB → `wled.local` just works in browser.**

## Architecture

```
Host PC                    Pi Pico                      ESP32 (WLED)
─────────────────────────────────────────────────────────────────────
        USB                         UART1
  169.254.7.2  ◄──CDC-NCM──►  169.254.7.3  ◄──PPP──►  169.254.7.1
  fe80::2         (Ethernet)   fe80::3       (921600)   fe80::1
  (DHCP client)                (IP forward)             (wled.local)
```

- **Host side**: CDC-NCM USB Ethernet — auto-detected on Linux, macOS, Windows 10+
- **ESP32 side**: PPP over UART at 921600 baud (configurable up to 5 Mbps)
- **Routing**: lwIP `IP_FORWARD=1` between USB netif and PPP netif
- **mDNS**: Pico answers `wled.local` queries with 169.254.7.1 (ESP32)
- **Proxy ARP**: Pico answers ARP for 169.254.7.1 so host reaches ESP32 on-link

## Why Link-Local (169.254.x.x)?

RFC 3927 link-local addressing (169.254.7.0/24) is used instead of RFC1918
private ranges (192.168.x.x, 10.x.x.x) because:

- **No conflicts**: Link-local can never collide with routed networks, VPNs,
  Docker/Podman subnets, BGP announcements, or corporate LANs
- **Purpose-built**: Designed for exactly this use case — point-to-point
  links with no router
- **Universal**: Works on every OS without routing table interference

## Network Configuration

| Interface      | IPv4 Address     | IPv6 Address | Role                         |
|----------------|------------------|--------------|------------------------------|
| USB (host)     | 169.254.7.2/24   | fe80::2/64   | Assigned by Pico DHCP server |
| USB (Pico)     | 169.254.7.3/24   | fe80::3/64   | Gateway + DHCP + mDNS        |
| PPP (Pico)     | 169.254.7.3/32   | fe80::3      | PPP client                   |
| PPP (ESP32)    | 169.254.7.1/32   | fe80::1      | WLED server (wled.local)     |

### Zero-Config Stack

1. **DHCP**: Host plugs USB → gets 169.254.7.2 with gateway 169.254.7.3
2. **Proxy ARP**: Host ARPs for 169.254.7.1 → Pico answers with its MAC
3. **IP Forward**: Pico forwards host→ESP32 traffic over PPP transparently
4. **mDNS**: Host queries `wled.local` → Pico responds with 169.254.7.1
5. **Browser**: `http://wled.local` → WLED web UI on ESP32

## Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x with submodules:
  ```bash
  git clone https://github.com/raspberrypi/pico-sdk.git
  cd pico-sdk
  git submodule update --init lib/tinyusb lib/lwip
  ```
- ARM cross-compiler: `arm-none-eabi-gcc`
  - Debian/Ubuntu: `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi`
  - Fedora/RHEL: `sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-newlib`
- CMake 3.20+
- GNU Make

## Build

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cd pico-bridge
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

Output: `build/pico_ncm_ppp_bridge.uf2`

## Flash

1. Hold the **BOOTSEL** button on the Pico
2. Connect the Pico to the host via USB while holding BOOTSEL
3. Release BOOTSEL — the Pico appears as a USB mass storage device (`RPI-RP2`)
4. Copy `pico_ncm_ppp_bridge.uf2` to the drive (or drag-and-drop)
5. The Pico reboots and starts the bridge firmware

## Wiring (Pico → M5StickC / ESP32)

| Pico Pin | Function    | ESP32 Pin | M5StickC Pin |
|----------|-------------|-----------|--------------|
| GP4      | UART1 TX    | UART RX   | G33          |
| GP5      | UART1 RX    | UART TX   | G32          |
| GND      | Ground      | GND       | GND          |

UART settings: 921600 baud, 8N1, no flow control.

To change pins or baud rate, edit the defines at the top of `src/main.c`:

```c
#define BRIDGE_UART_ID      uart1
#define BRIDGE_UART_TX_PIN  4
#define BRIDGE_UART_RX_PIN  5
#define BRIDGE_UART_BAUD    921600
```

## Debug Output

Debug messages go to UART0 (GP0/GP1) at 115200 baud. Connect a USB-serial
adapter to see boot messages and PPP status:

```
WLED USB Bridge started
USB: 169.254.7.3/24, DHCP: 169.254.7.2
ESP32: 169.254.7.1 (wled.local via mDNS)
UART1: 921600 baud (GP4 TX, GP5 RX)
PPP up: local=169.254.7.3 peer=169.254.7.1
```

## Components

- **TinyUSB** CDC-NCM device class (`CFG_TUD_NCM=1`)
- **lwIP 2.x** with PPP client, IP forwarding, IPv4+IPv6 dual-stack
- Built-in DHCP server (single client, 169.254.7.2)
- mDNS responder (`wled.local` → 169.254.7.1)
- Proxy ARP (ESP32 IP reachable on USB Ethernet)
- IRQ-driven UART RX with 4KB ring buffer
- Windows 10+ BOS/MS OS 2.0 descriptors for automatic WINNCM driver loading

## License

MIT — see individual source file headers.
