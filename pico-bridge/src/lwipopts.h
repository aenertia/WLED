/*
 * lwIP configuration for Pi Pico NCM-PPP Bridge
 * Dual netif: USB NCM (Ethernet) + UART PPP, with IP forwarding between them.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* No OS - bare metal */
#define NO_SYS                      1
#define MEM_ALIGNMENT               4
#define LWIP_SINGLE_NETIF           0  /* TWO netifs: USB + PPP */

/* IPv4 only */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_RAW                    0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_DHCP                   0  /* We provide DHCP server, not client */
#define LWIP_DNS                    0

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

/* Memory pool */
#define PBUF_POOL_SIZE              12
#define MEMP_NUM_TCP_SEG            16
#define MEM_SIZE                    (8 * 1024)

/* ARP */
#define LWIP_ARP                    1
#define ARP_TABLE_SIZE              4
#define ARP_QUEUEING                1

/* PPP support */
#define PPP_SUPPORT                 1
#define PPPOS_SUPPORT               1
#define PPP_IPV4_SUPPORT            1
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
#define PPP_INPROC_IRQ_SAFE         0  /* safe to call pppos_input from IRQ context */

/* Misc */
#define LWIP_PROVIDE_ERRNO          1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_CHKSUM_ALGORITHM       3

/* Debug (disable for production) */
#define LWIP_DEBUG                  0

#endif /* LWIPOPTS_H */
