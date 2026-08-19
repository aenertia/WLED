/*
 * lwIP options for Pi Pico NCM-to-PPP NAT bridge.
 *
 * Two netifs: CDC-NCM (Ethernet, host-facing) + PPPoS (serial, ESP32-facing).
 * Pico terminates connections on NCM side and relays/NATs to ESP32 via PPP.
 * NO_SYS=1 (bare-metal, no RTOS).
 */
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

/* ===== System ===== */
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        0
#define MEM_ALIGNMENT               4

/* ===== Memory ===== */
#define MEM_SIZE                    (16 * 1024)
#define MEMP_NUM_PBUF               24
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            24
#define PBUF_POOL_SIZE              24
#define PBUF_POOL_BUFSIZE           1600

/* ===== Core protocols ===== */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DNS                    0
#define LWIP_DHCP                   0       /* We ARE the DHCP server, not client */
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   1

/* ===== TCP tuning ===== */
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define LWIP_TCP_SACK_OUT           1
#define LWIP_WND_SCALE              1
#define TCP_RCV_SCALE               2

/* ===== ARP / Ethernet (NCM side) ===== */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define ARP_TABLE_SIZE              4       /* Only 1-2 hosts on NCM */
#define ETHARP_TABLE_MATCH_NETIF    1

/* ===== PPP ===== */
#define PPP_SUPPORT                 1
#define PPPOS_SUPPORT               1       /* PPP over Serial */
#define PPP_SERVER                  0       /* Pico is PPP CLIENT (ESP32 is server) */
#define PAP_SUPPORT                 0       /* No auth on this link */
#define CHAP_SUPPORT                0
#define MSCHAP_SUPPORT              0
#define EAP_SUPPORT                 0
#define CCP_SUPPORT                 0
#define MPPE_SUPPORT                0
#define VJ_SUPPORT                  0       /* Known broken with optimizations */
#define PPP_IPV4_SUPPORT            1
#define PPP_IPV6_SUPPORT            0
#define PPP_INPROC_IRQ_SAFE         0       /* We feed from main loop, not IRQ */
#define PPP_MRU                     1500
#define PPP_NOTIFY_PHASE            1
#define PPP_FCS_TABLE               1       /* 512B table for faster FCS */
#define LCP_ECHOINTERVAL            3       /* Match ESP32 sdkconfig */
#define LCP_MAXECHOFAILS            3

/* ===== Netif ===== */
#define LWIP_SINGLE_NETIF           0       /* TWO netifs: NCM + PPP */
#define IP_FORWARD                  0       /* NO forwarding  -- we do NAT */
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

/* ===== Fragmentation ===== */
#define IP_FRAG                     1
#define IP_REASSEMBLY               1
#define IP_REASS_MAX_PBUFS          8

/* ===== Misc ===== */
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

/* ===== Debug (disable for production) ===== */
#if 0
#define LWIP_DEBUG                  1
#define PPP_DEBUG                   LWIP_DBG_ON
#define TCP_DEBUG                   LWIP_DBG_ON
#define UDP_DEBUG                   LWIP_DBG_ON
#define ETHARP_DEBUG                LWIP_DBG_ON
#endif

#endif /* _LWIPOPTS_H */
