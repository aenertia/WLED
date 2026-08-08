# Cross-Platform USB Gadget Networking Quirks

Reference for Pi Pico NCM-PPP bridge firmware. Patterns learned from
ROCKNIX USB gadget networking (aenertia/distribution).

## 1. Windows NCM Auto-Detection (WINNCM + MS OS 2.0)

**Problem:** Windows 10/11 does not auto-load the NCM driver for unknown
USB devices. Users must manually install via Device Manager.

**Solution:** USB BOS descriptor advertises MS OS 2.0 Platform Capability.
When Windows queries the vendor-specific descriptor, the device returns a
Compatible ID descriptor with `WINNCM` plus a registry property for
`DeviceInterfaceGUIDs`. Windows then loads `UsbNcm.sys` automatically.

**Implementation:**
- `TUD_BOS_MS_OS_20_DESCRIPTOR()` in BOS with vendor code
- MS OS 2.0 descriptor set: Set Header → Config Subset → Function Subset →
  Compatible ID (`WINNCM`) → Registry Property (`DeviceInterfaceGUIDs`)
- `tud_vendor_control_xfer_cb()` handles the vendor request

**References:**
- Microsoft OS 2.0 Descriptors Specification
- TinyUSB `examples/device/net_lwip_webserver`
- ROCKNIX `distributions/ROCKNIX/options` (configfs WINNCM setup)

## 2. DHCP Option 249 — Microsoft Static Routes (msstaticroutes)

**Problem:** Windows marks USB NCM connections as "Unidentified Network"
with Public firewall profile. This blocks mDNS, LLMNR, and most discovery
protocols. The root cause: Windows cannot identify the network because
there is no default gateway route from a "real" router.

**Solution:** DHCP option 249 (Microsoft Classless Static Routes) provides
a default route (0.0.0.0/0) pointing to the Pico's IP (169.254.7.3).
Windows uses this to "identify" the network and assign Private profile.

**Format:**
```
Option 249, Length 5
  Prefix length: 0          (0.0.0.0/0 = default route)
  Gateway:       169.254.7.3 (4 bytes, network order)
```

**Note:** Option 249 is Microsoft-proprietary. RFC 3442 (option 121) is
the standard equivalent but Windows ignores it for network identification.
Both can coexist; we send 249 only since the sole consumer is Windows.

**References:**
- MS-DHCPE (Microsoft DHCP Extensions)
- ROCKNIX `packages/network/udhcpd-config`

## 3. Static MAC Address

**Problem:** Random or serial-derived MACs cause Windows/macOS/Linux to
treat each reconnect as a "new" network adapter. This resets:
- Windows network profile (Public/Private)
- macOS network location
- Linux NetworkManager connection UUID
- Firewall rules bound to interface identity

**Solution:** Fixed locally-administered MAC: `02:57:4C:ED:07:01`
- Byte 0 = 0x02: locally administered (bit 1), unicast (bit 0 = 0)
- Bytes 1-2: 'W' (0x57), 'L' (0x4C) — WLED branding
- Byte 3: 0xED — phonetic "ED"
- Bytes 4-5: subnet .7, device .1

The host-facing MAC is XORed with 0x01 on the last byte (TinyUSB
convention for CDC-NCM: device MAC vs host MAC differ by 1 bit).

**References:**
- IEEE 802-2014 §8.2 (locally administered addresses)
- ROCKNIX `packages/network/usb-gadget` (static MAC in configfs)

## 4. Gratuitous mDNS Announcements (RFC 6762 §8.3)

**Problem:** After USB plug-in or PPP link-up, the host has no mDNS cache
entry for `wled.local`. The user must wait for a query or timeout before
discovery works. On some systems (Windows), mDNS queries are delayed up
to 5 seconds after link-up.

**Solution:** Send 3 unsolicited mDNS announcements at 1-second intervals
immediately on link-up events:
1. PPP PPPERR_NONE (ESP32 reachable)
2. USB NCM re-enumeration (host reconnected)

Each announcement includes:
- IPv4 multicast to 224.0.0.251:5353 (A record for wled.local)
- IPv6 multicast to ff02::fb:5353 (same A record via IPv6 transport)
- Gratuitous ARP for 169.254.7.1 (pre-populate host ARP cache)

The 3x burst at 1s intervals follows RFC 6762 §8.3 recommended practice.
Periodic re-announcements every 60s maintain cache freshness.

**Implementation:** Timer-based burst in main loop (non-blocking). Counter
`mdns_announce_remaining` + timestamp `mdns_announce_next_ms` checked
each iteration. No `sleep_ms()` in callbacks.

**References:**
- RFC 6762 §8.3 (Multicast DNS Probing and Announcing)
- ROCKNIX `packages/network/avahi` (gratuitous on interface up)

## 5. Multicast Group Membership (IGMP + MLD6)

**Problem:** mDNS requires multicast group membership. Without it, the
network stack may not process incoming multicast packets, and some switches
(USB-Ethernet adapters with hub chips) filter unjoined groups.

**Solution:**
- `NETIF_FLAG_IGMP` on USB netif (IPv4 multicast)
- `NETIF_FLAG_MLD6` on USB netif (IPv6 multicast)
- `igmp_joingroup()` for 224.0.0.251 (mDNS IPv4)
- `mld6_joingroup()` for ff02::fb (mDNS IPv6)

**References:**
- RFC 2236 (IGMPv2)
- RFC 3810 (MLDv2)

## 6. Architecture: Pico Owns All Host-Facing Complexity

The ESP32 runs WLED and a minimal PPP server with static IPs. ALL
cross-platform concerns live on the Pico:

| Concern                  | Owner |
|--------------------------|-------|
| USB NCM descriptors      | Pico  |
| MS OS 2.0 / WINNCM      | Pico  |
| DHCP server              | Pico  |
| DHCP opt 249             | Pico  |
| mDNS responder           | Pico  |
| Gratuitous announcements | Pico  |
| Proxy ARP                | Pico  |
| IP forwarding            | Pico  |
| Static MAC               | Pico  |
| PPP client               | Pico  |

The ESP32 link is dead-simple: PPP with hardcoded 169.254.7.1 (ESP32) and
169.254.7.3 (Pico peer), no DHCP, no mDNS, no routing, no USB.
