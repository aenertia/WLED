# Pi Pico NCM-PPP Bridge

Firmware for Raspberry Pi Pico that bridges USB CDC-NCM Ethernet to UART PPP,
providing zero-config USB networking from a host computer to an ESP32 running WLED.

## Architecture

```
Host PC                    Pi Pico                      ESP32 (WLED)
─────────────────────────────────────────────────────────────────────
        USB                         UART1
  192.168.7.2  ◄──CDC-NCM──►  192.168.7.1  ◄──PPP──►  negotiated
  (DHCP client)    (Ethernet)   (IP forward)  (921600)  (PPP server)
```

- **Host side**: CDC-NCM USB Ethernet — auto-detected on Linux, macOS, Windows 10+
- **ESP32 side**: PPP over UART at 921600 baud (configurable up to 5 Mbps)
- **Routing**: lwIP `IP_FORWARD=1` between USB netif and PPP netif

The host receives IP 192.168.7.2 via the Pico's built-in DHCP server, with
gateway 192.168.7.1 (the Pico). Traffic destined for the ESP32's PPP address
is forwarded through the Pico automatically.

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

## Network Configuration

| Interface  | IP Address     | Role                           |
|------------|----------------|--------------------------------|
| USB (host) | 192.168.7.2/24 | Assigned by Pico DHCP server   |
| USB (Pico) | 192.168.7.1/24 | Gateway for host               |
| PPP (Pico) | negotiated     | PPP client                     |
| PPP (ESP32)| negotiated     | PPP server (WLED)              |

The host accesses WLED at the ESP32's PPP-negotiated IP address.
If the ESP32 PPP server assigns 10.7.0.2, then WLED is at `http://10.7.0.2`.

## Debug Output

Debug messages go to UART0 (GP0/GP1) at 115200 baud. Connect a USB-serial
adapter to see boot messages and PPP status:

```
WLED USB Bridge started
USB: 192.168.7.1/24, DHCP: 192.168.7.2
UART1: 921600 baud (GP4 TX, GP5 RX)
PPP up: 10.7.0.1
```

## Components

- **TinyUSB** CDC-NCM device class (`CFG_TUD_NCM=1`)
- **lwIP 2.x** with PPP client (`PPPOS_SUPPORT=1`) and IP forwarding (`IP_FORWARD=1`)
- Built-in DHCP server (single client, 192.168.7.2)
- IRQ-driven UART RX with 4KB ring buffer
- Windows 10+ BOS/MS OS 2.0 descriptors for automatic WINNCM driver loading

## License

MIT — see individual source file headers.
