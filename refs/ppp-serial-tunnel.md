# PPP-over-Serial Tunnel Reference

## Why PPP, Not SLIP

| Factor | SLIP | PPP |
|---|---|---|
| ESP-IDF support | **Removed** from `esp_netif` in v5.1 (Aug 2022) | First-class, mature, used in every cellular project |
| IP negotiation | None — manual config both sides | LCP/IPCP auto-negotiates IP, MTU, compression |
| Error detection | None | FCS (Frame Check Sequence) per frame |
| VJ compression | No | Yes — saves ~20 bytes/packet on small UDP |
| Linux host | `slattach` (manual IP config) | `pppd` (auto-configures, NetworkManager) |
| Windows | **No native support since XP** | Built-in DUN / third-party `pppd` |
| macOS | Removed `slattach` | `pppd` built-in |

**SLIP was removed from ESP-IDF core** — the `ESP_NETIF_FLAG_IS_SLIP`, `ESP_NETIF_DEFAULT_SLIP()`, and `esp_netif_lwip_slip.c` were all deleted in commit `83b8556`. The lwIP `slipif` code still compiles with `CONFIG_LWIP_SLIP_SUPPORT`, but there's no `esp_netif` integration. Dead end.

## ESP-IDF PPP API

### Kconfig Requirements

```
CONFIG_LWIP_PPP_SUPPORT=y
CONFIG_LWIP_PPP_SERVER_SUPPORT=y
```

In PlatformIO `sdkconfig.defaults` or `platformio.ini` build flags:
```ini
build_flags =
  -D CONFIG_LWIP_PPP_SUPPORT=1
  -D CONFIG_LWIP_PPP_SERVER_SUPPORT=1
```

### PPP Server Configuration

From `esp_netif_ppp.h`:

```c
typedef struct esp_netif_ppp_config {
    bool ppp_phase_event_enabled;
    bool ppp_error_event_enabled;
    esp_ip4_addr_t ppp_our_ip4_addr;    // ESP32's IP (10.0.0.1)
    esp_ip4_addr_t ppp_their_ip4_addr;  // Host PC's IP (10.0.0.2)
    esp_ip4_addr_t ppp_dns1_addr;
    esp_ip4_addr_t ppp_dns2_addr;
    bool ppp_passive;                    // true = server mode (ppp_listen)
} esp_netif_ppp_config_t;
```

### PPP Netif Creation Pattern

From ESP-IDF `ppp_connect.c` example:

```c
// Transmit callback — called by lwIP PPP to send frames
static esp_err_t ppp_transmit(void *h, void *buffer, size_t len) {
    uart_write_bytes(UART_NUM_0, buffer, len);
    return ESP_OK;
}

// UART RX task — feeds received bytes to PPP stack
static void ppp_rx_task(void *args) {
    uint8_t buffer[1024];
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
        if (len > 0) {
            esp_netif_receive(s_ppp_netif, buffer, len, NULL);
        }
    }
}

// Setup
void ppp_init(void) {
    // UART config
    uart_config_t uart_config = {
        .baud_rate = 1500000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_driver_install(UART_NUM_0, 2048, 2048, 20, &uart_event_queue, 0);

    // PPP netif
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    esp_netif_config_t cfg = { .base = &base_cfg, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP };
    s_ppp_netif = esp_netif_new(&cfg);

    // Set server mode
    esp_netif_ppp_config_t ppp_config = {
        .ppp_phase_event_enabled = true,
        .ppp_error_event_enabled = true,
        .ppp_our_ip4_addr = { .addr = ESP_IP4TOADDR(10, 0, 0, 1) },
        .ppp_their_ip4_addr = { .addr = ESP_IP4TOADDR(10, 0, 0, 2) },
        .ppp_passive = true,  // Server mode — wait for host pppd
    };
    esp_netif_ppp_set_params(s_ppp_netif, &ppp_config);

    // Register transmit callback
    // ... (varies by ESP-IDF version, see esp_netif_new_with_config)

    // Start PPP
    esp_netif_action_start(s_ppp_netif, 0, 0, 0);

    // Start RX task
    xTaskCreate(ppp_rx_task, "ppp_rx", 4096, NULL, 5, NULL);
}
```

### `eppp_link` Component (Espressif Official)

Repo: `espressif/esp-protocols/components/eppp_link`

Higher-level wrapper that handles UART setup, PPP negotiation, and channel multiplexing:

```c
#include "eppp_link.h"

eppp_config_t eppp_config = EPPP_DEFAULT_CONFIG(UART_NUM_0, 1500000);
esp_netif_t *ppp_netif = eppp_listen(&eppp_config);
// That's it — netif is up, pppd on host can connect
```

Measured throughput: UART @ 3 Mbaud → 2 Mbps TCP/UDP (66% efficiency).

## Host-Side Setup

### Linux — `pppd` (Recommended)

```bash
# Basic connection
sudo pppd /dev/ttyUSB0 1500000 \
    noauth local nocrtscts nodetach \
    10.0.0.2:10.0.0.1

# After connection:
# - ppp0 interface created
# - Local IP: 10.0.0.2
# - Remote (ESP32): 10.0.0.1
# - Browse to http://10.0.0.1
# - OpenRGB DDP to 10.0.0.1:4048
```

### Linux — systemd auto-connect on USB plug-in

```ini
# /etc/udev/rules.d/99-wled-ppp.rules
ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", \
    ATTRS{idProduct}=="6001", SYMLINK+="wled-serial", \
    TAG+="systemd", ENV{SYSTEMD_WANTS}="wled-ppp@%k.service"

# Ignore ModemManager for this device
ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", \
    ATTRS{idProduct}=="6001", ENV{ID_MM_DEVICE_IGNORE}="1"
```

```ini
# /etc/systemd/system/wled-ppp@.service
[Unit]
Description=PPP link to WLED ESP32 on /dev/%i
After=dev-%i.device
BindsTo=dev-%i.device

[Service]
Type=simple
ExecStart=/usr/sbin/pppd /dev/%i 1500000 noauth local nocrtscts \
    10.0.0.2:10.0.0.1 persist maxfail 0 holdoff 3
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### macOS

```bash
sudo pppd /dev/tty.usbserial-* 1500000 noauth local nocrtscts nodetach
```

### Windows

- Create Direct Cable Connection (DUN) via Network Settings
- Or use WSL2 with `pppd`
- Or userspace proxy (Python/Go serial-to-TCP bridge)

### Important: ModemManager

ModemManager WILL probe the FTDI serial port and interfere. Disable or exclude:

```bash
# Global disable
sudo systemctl stop ModemManager
sudo systemctl disable ModemManager

# Per-device exclude (via udev rule above)
ENV{ID_MM_DEVICE_IGNORE}="1"
```

## Throughput Analysis

### Raw Serial → Effective IP

| Serial Baud | PPP Overhead | Effective IP | Notes |
|---|---|---|---|
| 115200 | ~8% | ~106 kbps / 13.2 KB/s | Too slow for web UI |
| 500000 | ~8% | ~460 kbps / 57.5 KB/s | Usable |
| 1000000 | ~8% | ~920 kbps / 115 KB/s | Good |
| **1500000** | **~8%** | **~1.38 Mbps / 172 KB/s** | **Recommended** |

PPP overhead: HDLC framing (7E flags, address/control, protocol, FCS) = ~8 bytes/frame + ACCM byte escaping (~2-5%).

### WLED Traffic Budget @ 1.5 Mbps (172 KB/s effective)

| Traffic | Size | Frequency | Bandwidth | % of capacity |
|---|---|---|---|---|
| Dashboard initial load | ~55 KB | Once | Burst: 0.32s | — |
| WebSocket state updates | ~3 KB | Max 1/sec | 3 KB/s | 1.7% |
| WS live LED preview (300 LEDs) | ~900 B | 25 fps | 22.5 KB/s | 13% |
| DDP pixel data (300 LEDs, 30fps) | ~928 B | 30 fps | 27.8 KB/s | 16% |
| **Combined (all active)** | | | **~53.3 KB/s** | **31%** |

69% headroom remaining. Serial is not the bottleneck.

### TCP Round-Trip Latency

At 1.5 Mbps serial, a 1500-byte TCP segment takes ~10ms to transmit + ~10ms for ACK = ~20ms RTT. Web UI initial load involves ~5-6 HTTP round trips (with keep-alive) → expect **2-4 seconds** for full dashboard load. After that, WebSocket updates are sub-millisecond.

## WLED Integration — Following the Ethernet Pattern

WLED already supports Ethernet (non-WiFi) via `WLED_USE_ETHERNET`. The `WLEDNetworkClass` abstracts the transport:

```cpp
// Network.cpp
bool WLEDNetworkClass::isConnected() {
    return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED)
        || isEthernet();
}

bool WLEDNetworkClass::isEthernet() {
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
    return (ETH.localIP()[0] != 0) && ETH.linkUp();
#endif
    return false;
}
```

**Our PPP integration follows the same pattern:**

```cpp
bool WLEDNetworkClass::isConnected() {
    return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED)
        || isEthernet()
        || isPPP();  // Add this
}

bool WLEDNetworkClass::isPPP() {
#ifdef WLED_USE_PPP
    // Check if PPP netif has an IP assigned
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(ppp_netif, &ip_info) == ESP_OK) {
        return ip_info.ip.addr != 0;
    }
#endif
    return false;
}
```

Similarly extend `localIP()`, `subnetMask()`, `gatewayIP()` to return PPP interface addresses when PPP is the active transport.

### Files Changed (following Ethernet precedent)

| File | Change | Lines |
|---|---|---|
| `wled00/src/dependencies/network/Network.h` | Add `isPPP()` declaration | ~3 |
| `wled00/src/dependencies/network/Network.cpp` | Implement `isPPP()`, extend `isConnected()`, `localIP()` | ~30 |
| `wled00/wled.cpp` | PPP netif init in `WLED::setup()`, skip WiFi init if PPP | ~40 |
| `wled00/wled.h` | `WLED_USE_PPP` flag, PPP netif global | ~5 |
| `platformio_override.ini` | Kconfig flags, board env | new file |
| `usermods/m5stickc_ppp/` | M5StickC display, buttons, PPP UART setup | new dir |

**Total core change estimate: ~80 lines.** Less invasive than the original serial-only approach because we're adding a transport alongside Ethernet, not hacking serial handlers.

## Architecture Diagram

```
┌──────────────────────────────┐     USB Serial (1.5 Mbps)     ┌────────────────────────────┐
│   M5StickC (ESP32-PICO-D4)  │◄═════════════════════════════►│   Host PC (koero/z20)      │
│                              │     FTDI FT232                │                            │
│  ┌─────────────────────┐    │     PPP HDLC frames           │  pppd → ppp0 interface     │
│  │ WLED (stock)        │    │                               │  IP: 10.0.0.2              │
│  │  AsyncWebServer :80 │    │                               │                            │
│  │  WebSocket /ws      │    │                               │  Browser → http://10.0.0.1 │
│  │  DDP :4048 (UDP)    │    │                               │  OpenRGB → DDP 10.0.0.1    │
│  │  E1.31 :5568 (UDP)  │    │                               │  JSON API → POST /json     │
│  │  Effects engine     │    │                               │                            │
│  └────────┬────────────┘    │                               │  systemd auto-connect on   │
│           │                  │                               │  USB plug-in via udev rule │
│  ┌────────▼────────────┐    │                               └────────────────────────────┘
│  │ esp_netif PPP       │    │
│  │ IP: 10.0.0.1        │    │
│  │ ppp_listen() server │    │
│  └────────┬────────────┘    │
│           │                  │
│  ┌────────▼────────────┐    │
│  │ UART0 (FTDI)        │    │
│  │ 1500000 baud        │    │
│  └─────────────────────┘    │
│                              │
│  G26 (HAT) → 74AHCT125 → WS2812B ARGB strip/fans          │
└──────────────────────────────┘
```

## What This Eliminates

Everything from the original serial-only ADR that is now unnecessary:

| Was Needed | Now Replaced By | Notes |
|---|---|---|
| Custom serial JSON API extension | HTTP POST `/json` over PPP | Stock WLED |
| Adalight serial reception | DDP over UDP over PPP | Stock WLED, OpenRGB preferred |
| TPM2 serial reception | E1.31 over UDP over PPP | Stock WLED |
| Serial baud rate switching | Fixed 1.5 Mbps PPP link | No runtime switching |
| Serial RX buffer management | PPP + lwIP handle framing | Kernel-level |
| `wled-serial-cli` companion | `curl http://10.0.0.1/json` | Standard HTTP tools |
| Custom serial multiplexer | IP handles multiplexing | TCP + UDP coexist natively |
