# Iteration Wave 2: IDF v5 Rebase + PPP-over-Serial Implementation

## Context

Wave 1 confirmed:
- M5StickC builds clean on koero NVMe (PlatformIO + IDF v4)
- Lean build: 1.1MB firmware, 775KB flash headroom, 58% usage
- PPP headers exist in IDF 4.4.8 but liblwip.a prebuilt WITHOUT PPP
- **Solution: rebase onto upstream main which has IDF v5 as default**

Upstream WLED main (post-PR #4838) provides:
- `[esp32_idf_V5]` section: IDF 5.3.4, arduino-esp32 v3.1.10
- `framework = arduino` gives full IDF API access (sdkconfig.defaults works)
- lwIP compiles from source → PPP enabled via sdkconfig
- `WLED_ETHERNET_ONLY_BUILD` pattern from PR #5697 (our WiFi-bypass template)

All builds on koero NVMe at `/var/mnt/koero/workspace/wled/`.

## TODOs

- [ ] 1. Rebase onto upstream main — `git fetch upstream && git rebase upstream/main`, resolve conflicts in our files (platformio_override.ini, AGENTS.md, learnings.md, refs/, adr-wled.md, PLAN-*)
- [ ] 2. IDF v5 M5StickC build — update platformio_override.ini to extend esp32_idf_V5, build `pio run -e m5stickc`, verify firmware.bin exists and flash fits
- [ ] 3. IDF v5 lean build — update m5stickc_lean to extend V5 m5stickc env, add disable flags, build and compare sizes with Wave 1 V4 baselines
- [ ] 4. sdkconfig.defaults for PPP — create sdkconfig.defaults with CONFIG_LWIP_PPP_SUPPORT=y + CONFIG_LWIP_PPP_SERVER_SUPPORT=y + lwIP tuning, verify PPP compiles into firmware (nm check for ppp symbols in .elf)
- [ ] 5. PPP netif implementation — add WLED_USE_PPP flag following WLED_ETHERNET_ONLY_BUILD pattern from PR #5697, create PPP server netif on UART0, register IP_EVENT_PPP_GOT_IP handler to trigger interfacesInited, extend WLEDNetworkClass with isPPP()/localIP()/subnetMask()
- [ ] 6. Host-side pppd test — `sudo pppd /dev/ttyUSB0 1500000 noauth local nocrtscts nodetach`, verify IPCP negotiates 169.254.7.x link-local IPs, browse to http://wled.local (or http://169.254.7.1), confirm WLED dashboard loads
- [ ] 7. DDP pixel streaming — configure OpenRGB with DDP device at wled.local:4048, verify realtime LED control over PPP tunnel, measure latency
- [ ] 8. Commit, document, update AGENTS.md + learnings.md with V5 build findings and PPP implementation details

## Final Verification Wave

- [ ] F1. Clean-state `pio run -e m5stickc_ppp` succeeds from empty .pio/build/ on koero
- [ ] F2. Host pppd connects, wled.local resolves via mDNS, dashboard loads in browser
- [ ] F3. DDP pixel data from host reaches ESP32, LEDs update in realtime
- [ ] F4. Binary fits M5StickC 4MB flash with PPP stack included

## Key References

| Resource | Use |
|---|---|
| PR #5697 petrisy/WLED p4-eth-upstream-ready | WLED_ETHERNET_ONLY_BUILD pattern — our template for WiFi bypass |
| PR #4838 IDF V5 migration | What changed for V5 builds, NetworkClass rename |
| refs/ppp-serial-tunnel.md | ESP-IDF PPP API, esp_netif config, host pppd setup |
| refs/wled-community-intel.md | Community precedents, known issues, active refactoring |

## sdkconfig.defaults (to create in Task 4)

```
# PPP over Serial
CONFIG_LWIP_PPP_SUPPORT=y
CONFIG_LWIP_PPP_SERVER_SUPPORT=y
CONFIG_LWIP_PPP_PAP_SUPPORT=y
CONFIG_LWIP_PPP_ENABLE_IPV4=y
# CONFIG_LWIP_PPP_ENABLE_IPV6 is not set
CONFIG_LWIP_ENABLE_LCP_ECHO=y
CONFIG_LWIP_LCP_ECHOINTERVAL=3
CONFIG_LWIP_LCP_MAXECHOFAILS=3

# lwIP tuning for single PPP link
CONFIG_LWIP_MAX_SOCKETS=8
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=2880
CONFIG_LWIP_TCP_WND_DEFAULT=5744
```

## platformio_override.ini target (Task 2-4)

```ini
[env:m5stickc_ppp]
extends = esp32_idf_V5
board = m5stick-c
build_flags = ${common.build_flags} ${esp32_idf_V5.build_flags}
  -D WLED_RELEASE_NAME=\M5StickC_PPP\
  -D WLED_USE_PPP
  -D PPP_BAUD=1500000
  -D PPP_OUR_IP=\169.254.7.1\
  -D PPP_THEIR_IP=\169.254.7.2\
  -D MDNS_NAME=\wled\
  -D DATA_PINS=26
  -D DEFAULT_LED_COUNT=60
  -D BTNPIN=37
  -D WLED_DISABLE_ALEXA
  -D WLED_DISABLE_HUESYNC
  -D WLED_DISABLE_INFRARED
  -D WLED_DISABLE_MQTT
  -D WLED_DISABLE_OTA
  -D WLED_DISABLE_ESPNOW
  -D WLED_DISABLE_LOXONE
  -D WLED_DISABLE_ADALIGHT
  -D WLED_DISABLE_2D
  -DARDUINO_USB_CDC_ON_BOOT=0
lib_deps = ${esp32_idf_V5.lib_deps}
lib_ignore = ${esp32_idf_V5.lib_ignore}
board_build.partitions = ${esp32.big_partitions}
monitor_speed = 115200
```

## Success Criteria

- WLED dashboard accessible at http://wled.local over USB PPP link
- DDP pixel streaming works from host through PPP tunnel
- mDNS announces wled.local, appears in Dolphin/Avahi network browser
- Binary fits M5StickC 4MB flash with PPP stack
- All changes committed to Forgejo with updated AGENTS.md + learnings.md
