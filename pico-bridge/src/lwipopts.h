/*
 * lwIP configuration for Pi Pico NCM-PPP Bridge
 * Dual netif: USB NCM (Ethernet) + UART PPP, with IP forwarding between them.
 * Dual-stack: IPv4 (169.254.7.x link-local) + IPv6 (fe80:: link-local).
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* No OS - bare metal */
#define NO_SYS                      1
#define MEM_ALIGNMENT               4
#define LWIP_SINGLE_NETIF           0  /* TWO netifs: USB + PPP */

/* IPv4 */
#define LWIP_IPV4                   1
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_RAW                    0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_DHCP                   0  /* We provide DHCP server, not client */
#define LWIP_DNS                    0

/* IPv6 - dual-stack */
#define LWIP_IPV6                   1
#define LWIP_IPV6_AUTOCONFIG        1  /* SLAAC */
#define LWIP_IPV6_MLD               1  /* Multicast Listener Discovery (IPv6 mDNS) */
#define LWIP_IPV6_NUM_ADDRESSES     3
#define LWIP_ND6_LISTEN_RA          1

/* IP forwarding between USB and PPP netifs */
#define IP_FORWARD                  1

/* Multicast forwarding for mDNS across the bridge */
#define LWIP_IGMP                   1
#define LWIP_MULTICAST_TX_OPTIONS   1

/* Ethernet */
#define ETH_PAD_SIZE                0
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_STATUS_CALLBACK  1

/* TCP tuning */
#define TCP_MSS                     (1500 - 40)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_WND                     (8 * TCP_MSS)

/* Memory pool — increased for IPv6 + mDNS + DHCP */
#define PBUF_POOL_SIZE              16
#define MEMP_NUM_TCP_SEG            16
#define MEM_SIZE                    (12 * 1024)

/* Timeouts — extra headroom for PPP + mDNS + IPv6 + DHCP */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

/* ARP */
#define LWIP_ARP                    1
#define ARP_TABLE_SIZE              4
#define ARP_QUEUEING                1

/* Accept UDP on DHCP server port before netif has address */
#define LWIP_IP_ACCEPT_UDP_PORT(dst_port) ((dst_port) == 67)

/* PPP support */
#define PPP_SUPPORT                 1
#define PPPOS_SUPPORT               1
#define PPP_IPV4_SUPPORT            1
#define PPP_IPV6_SUPPORT            1  /* IPV6CP — graceful fallback if peer unsupported */
#define PAP_SUPPORT                 0
#define CHAP_SUPPORT                0
#define PPP_SERVER                  0  /* Pico is PPP CLIENT */
#define VJ_SUPPORT                  0
#define CCP_SUPPORT                 0
#define ECP_SUPPORT                 0
#define MPPE_SUPPORT                0
#define PPP_NOTIFY_PHASE            1
#define LCP_ECHOINTERVAL            10
#define LCP_MAXECHOFAILS            3
#define PPP_INPROC_IRQ_SAFE         0

/* Misc */
#define LWIP_PROVIDE_ERRNO          1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_CHKSUM_ALGORITHM       3

/* Debug (disable for production) */
#define LWIP_DEBUG                  0

struct pbuf;
struct netif;
int ddp_hook_ip_input(struct pbuf *p, struct netif *inp, int is_v6);
#define LWIP_HOOK_IP4_INPUT(p,inp) ddp_hook_ip_input(p, inp, 0)
#define LWIP_HOOK_IP6_INPUT(p,inp)      ddp_hook_ip_input(p, inp, 1)

#endif /* LWIPOPTS_H */
