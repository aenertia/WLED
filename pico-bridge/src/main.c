/*
 * Pi Pico NCM-PPP Bridge Firmware
 *
 * Presents CDC-NCM USB Ethernet to the host computer.
 * Bridges IP traffic over UART PPP to an ESP32 running WLED.
 *
 * Architecture:
 *   Host <--USB CDC-NCM--> Pico <--UART PPP--> ESP32 (WLED)
 *
 * Two lwIP netifs with IP_FORWARD=1:
 *   - usb_netif:  169.254.7.3/24  (Ethernet, faces host)
 *   - ppp_netif:  169.254.7.3/32  (PPP client, faces ESP32 at 169.254.7.1)
 *
 * The host gets 169.254.7.2 via built-in DHCP server.
 * mDNS responds to wled.local with 169.254.7.1 (ESP32).
 * Proxy ARP makes 169.254.7.1 reachable from host's /24 subnet.
 * IPv6 link-local (fe80::) on all interfaces.
 *
 * Addressing uses RFC 3927 link-local (169.254.x.x) — no conflict with
 * any routed network, BGP, VPN, or container subnet.
 *
 * UART1 pins (configurable):
 *   GP4 = TX -> ESP32 RX
 *   GP5 = RX <- ESP32 TX
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/uart.h"
#include "hardware/irq.h"

#include "bsp/board_api.h"
#include "tusb.h"

#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/ip.h"
#include "lwip/igmp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/mld6.h"
#include "lwip/ethip6.h"
#include "netif/ethernet.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#ifndef BRIDGE_UART_ID
  #define BRIDGE_UART_ID      uart1
#endif
#ifndef BRIDGE_UART_TX_PIN
  #define BRIDGE_UART_TX_PIN  4
#endif
#ifndef BRIDGE_UART_RX_PIN
  #define BRIDGE_UART_RX_PIN  5
#endif
#ifndef BRIDGE_UART_BAUD
  #define BRIDGE_UART_BAUD    921600
#endif

/* Link-local addressing (RFC 3927) — 169.254.7.0/24 */
#define USB_IP_ADDR      LWIP_MAKEU32(169, 254, 7, 3)   /* Pico USB */
#define USB_NETMASK      LWIP_MAKEU32(255, 255, 255, 0)
#define USB_GW           LWIP_MAKEU32(0, 0, 0, 0)
#define ESP32_IP_ADDR    LWIP_MAKEU32(169, 254, 7, 1)   /* ESP32 WLED */

/* DHCP pool — single host client */
#define DHCP_CLIENT_IP   LWIP_MAKEU32(169, 254, 7, 2)
#define DHCP_GATEWAY_IP  LWIP_MAKEU32(169, 254, 7, 3)
#define DHCP_SERVER_PORT 67

/* UART RX ring buffer */
#define UART_RX_BUF_SIZE 4096

/* LED blink */
#define BLINK_INTERVAL_MS 500

/* mDNS */
#define MDNS_PORT        5353
#define MDNS_TTL         120  /* seconds */

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static struct netif usb_netif;
static struct netif ppp_netif_data;
static ppp_pcb *ppp_pcb_inst;

static volatile uint8_t  uart_rx_buf[UART_RX_BUF_SIZE];
static volatile uint32_t uart_rx_head;
static volatile uint32_t uart_rx_tail;

static struct pbuf *received_frame;

uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};

static volatile bool ppp_connected;

/* ------------------------------------------------------------------ */
/* Minimal DHCP Server (single-client, fixed 169.254.7.2)              */
/* ------------------------------------------------------------------ */

#define DHCP_OP_REQUEST   1
#define DHCP_OP_REPLY     2
#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SRV_ID   54
#define DHCP_OPT_LEASE    51
#define DHCP_OPT_SUBNET   1
#define DHCP_OPT_ROUTER   3
#define DHCP_OPT_END      0xFF
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5

struct dhcp_msg_t {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;
    uint8_t  options[312];
};

static struct udp_pcb *dhcp_pcb;

static uint8_t dhcp_get_msg_type(const uint8_t *options, uint16_t len) {
    for (uint16_t i = 0; i < len - 2;) {
        uint8_t opt = options[i];
        if (opt == DHCP_OPT_END) break;
        if (opt == 0x00) { i++; continue; }
        uint8_t olen = options[i + 1];
        if (opt == DHCP_OPT_MSG_TYPE && olen == 1) return options[i + 2];
        i += 2 + olen;
    }
    return 0;
}

static void dhcp_add_opt_u8(uint8_t *buf, int *oi, uint8_t code, uint8_t val) {
    buf[(*oi)++] = code;
    buf[(*oi)++] = 1;
    buf[(*oi)++] = val;
}

static void dhcp_add_opt_u32(uint8_t *buf, int *oi, uint8_t code, uint32_t val) {
    buf[(*oi)++] = code;
    buf[(*oi)++] = 4;
    uint32_t nval = PP_HTONL(val);
    memcpy(&buf[*oi], &nval, 4);
    *oi += 4;
}

static void dhcp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)addr; (void)port;

    if (p->tot_len < 240) { pbuf_free(p); return; }

    struct dhcp_msg_t msg;
    uint16_t copy_len = p->tot_len;
    if (copy_len > sizeof(msg)) copy_len = sizeof(msg);
    memset(&msg, 0, sizeof(msg));
    pbuf_copy_partial(p, &msg, copy_len, 0);
    pbuf_free(p);

    if (msg.op != DHCP_OP_REQUEST) return;
    if (msg.cookie != PP_HTONL(DHCP_MAGIC_COOKIE)) return;

    uint16_t opts_len = (copy_len > 240) ? (copy_len - 240) : 0;
    uint8_t msg_type = dhcp_get_msg_type(msg.options, opts_len);
    if (msg_type != DHCP_MSG_DISCOVER && msg_type != DHCP_MSG_REQUEST) return;

    /* Build reply */
    struct dhcp_msg_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.op     = DHCP_OP_REPLY;
    reply.htype  = 1;
    reply.hlen   = 6;
    reply.xid    = msg.xid;
    reply.yiaddr = PP_HTONL(DHCP_CLIENT_IP);
    reply.siaddr = PP_HTONL(USB_IP_ADDR);
    memcpy(reply.chaddr, msg.chaddr, 16);
    reply.cookie = PP_HTONL(DHCP_MAGIC_COOKIE);

    int oi = 0;
    uint8_t reply_type = (msg_type == DHCP_MSG_DISCOVER)
                         ? DHCP_MSG_OFFER : DHCP_MSG_ACK;
    dhcp_add_opt_u8(reply.options, &oi, DHCP_OPT_MSG_TYPE, reply_type);
    dhcp_add_opt_u32(reply.options, &oi, DHCP_OPT_SRV_ID, USB_IP_ADDR);
    dhcp_add_opt_u32(reply.options, &oi, DHCP_OPT_LEASE, 86400);
    dhcp_add_opt_u32(reply.options, &oi, DHCP_OPT_SUBNET, USB_NETMASK);
    dhcp_add_opt_u32(reply.options, &oi, DHCP_OPT_ROUTER, DHCP_GATEWAY_IP);
    reply.options[oi++] = DHCP_OPT_END;

    struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, sizeof(reply), PBUF_RAM);
    if (rp) {
        memcpy(rp->payload, &reply, sizeof(reply));
        ip_addr_t bcast = IPADDR4_INIT_BYTES(255, 255, 255, 255);
        udp_sendto_if(pcb, rp, &bcast, 68, &usb_netif);
        pbuf_free(rp);
    }
}

static void dhcp_server_init(void) {
    dhcp_pcb = udp_new();
    if (!dhcp_pcb) return;
    ip_addr_t any;
    ip_addr_set_any(false, &any);
    udp_bind(dhcp_pcb, &any, DHCP_SERVER_PORT);
    udp_recv(dhcp_pcb, dhcp_server_recv, NULL);
}

/* ------------------------------------------------------------------ */
/* Proxy ARP — answer ARP requests for ESP32 (169.254.7.1) with       */
/* the Pico's USB MAC so host traffic gets forwarded via PPP.          */
/* ------------------------------------------------------------------ */

static err_t usb_linkoutput_fn(struct netif *netif, struct pbuf *p);

static bool handle_proxy_arp(struct pbuf *p) {
    if (p->tot_len < (SIZEOF_ETH_HDR + sizeof(struct etharp_hdr)))
        return false;

    struct eth_hdr *ethhdr = (struct eth_hdr *)p->payload;
    if (ethhdr->type != PP_HTONS(ETHTYPE_ARP))
        return false;

    struct etharp_hdr *arphdr =
        (struct etharp_hdr *)((uint8_t *)p->payload + SIZEOF_ETH_HDR);
    if (arphdr->opcode != PP_HTONS(ARP_REQUEST))
        return false;

    /* Is this asking for the ESP32's IP? */
    uint32_t target_ip;
    SMEMCPY(&target_ip, &arphdr->dipaddr, sizeof(target_ip));
    if (target_ip != PP_HTONL(ESP32_IP_ADDR))
        return false;

    /* Build proxy ARP reply: "169.254.7.1 is at <Pico USB MAC>" */
    struct pbuf *reply = pbuf_alloc(PBUF_RAW,
        SIZEOF_ETH_HDR + sizeof(struct etharp_hdr), PBUF_RAM);
    if (!reply) return false;

    struct eth_hdr *rep_eth = (struct eth_hdr *)reply->payload;
    struct etharp_hdr *rep_arp =
        (struct etharp_hdr *)((uint8_t *)reply->payload + SIZEOF_ETH_HDR);

    /* Ethernet header */
    SMEMCPY(&rep_eth->dest, &ethhdr->src, sizeof(struct eth_addr));
    SMEMCPY(&rep_eth->src, usb_netif.hwaddr, sizeof(struct eth_addr));
    rep_eth->type = PP_HTONS(ETHTYPE_ARP);

    /* ARP reply */
    rep_arp->hwtype  = PP_HTONS(1);
    rep_arp->proto   = PP_HTONS(ETHTYPE_IP);
    rep_arp->hwlen   = 6;
    rep_arp->protolen = 4;
    rep_arp->opcode  = PP_HTONS(ARP_REPLY);
    SMEMCPY(&rep_arp->shwaddr, usb_netif.hwaddr, sizeof(struct eth_addr));
    SMEMCPY(&rep_arp->sipaddr, &arphdr->dipaddr, sizeof(arphdr->sipaddr));
    SMEMCPY(&rep_arp->dhwaddr, &arphdr->shwaddr, sizeof(struct eth_addr));
    SMEMCPY(&rep_arp->dipaddr, &arphdr->sipaddr, sizeof(arphdr->sipaddr));

    usb_linkoutput_fn(&usb_netif, reply);
    pbuf_free(reply);
    return false;  /* let lwIP also process it for its own ARP table */
}

/* Send gratuitous ARP for ESP32 IP so host caches it immediately */
static void send_gratuitous_arp(void) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW,
        SIZEOF_ETH_HDR + sizeof(struct etharp_hdr), PBUF_RAM);
    if (!p) return;

    struct eth_hdr *ethhdr = (struct eth_hdr *)p->payload;
    struct etharp_hdr *arphdr =
        (struct etharp_hdr *)((uint8_t *)p->payload + SIZEOF_ETH_HDR);

    /* Broadcast Ethernet */
    memset(&ethhdr->dest, 0xFF, sizeof(struct eth_addr));
    SMEMCPY(&ethhdr->src, usb_netif.hwaddr, sizeof(struct eth_addr));
    ethhdr->type = PP_HTONS(ETHTYPE_ARP);

    /* Gratuitous ARP: sender=ESP32 IP+Pico MAC, target=ESP32 IP+broadcast */
    arphdr->hwtype  = PP_HTONS(1);
    arphdr->proto   = PP_HTONS(ETHTYPE_IP);
    arphdr->hwlen   = 6;
    arphdr->protolen = 4;
    arphdr->opcode  = PP_HTONS(ARP_REPLY);
    SMEMCPY(&arphdr->shwaddr, usb_netif.hwaddr, sizeof(struct eth_addr));
    uint32_t esp_ip = PP_HTONL(ESP32_IP_ADDR);
    SMEMCPY(&arphdr->sipaddr, &esp_ip, sizeof(arphdr->sipaddr));
    memset(&arphdr->dhwaddr, 0xFF, sizeof(struct eth_addr));
    SMEMCPY(&arphdr->dipaddr, &esp_ip, sizeof(arphdr->dipaddr));

    usb_linkoutput_fn(&usb_netif, p);
    pbuf_free(p);
}

/* ------------------------------------------------------------------ */
/* mDNS responder — answers wled.local with 169.254.7.1 (ESP32)       */
/* ------------------------------------------------------------------ */

/*
 * Minimal mDNS: listen on UDP 5353 for queries about "wled.local",
 * respond with A record pointing to the ESP32's IP.  Only one name,
 * only one client subnet — no need for lwIP's full mDNS app.
 */

/* Pre-encoded DNS name: \x04wled\x05local\x00 */
static const uint8_t mdns_name_wled[] = {4,'w','l','e','d', 5,'l','o','c','a','l', 0};
#define MDNS_NAME_LEN sizeof(mdns_name_wled)

static struct udp_pcb *mdns_pcb;

/* Check if question matches wled.local (case-insensitive) */
static bool mdns_name_match(const uint8_t *qname, uint16_t qlen) {
    if (qlen < MDNS_NAME_LEN) return false;
    for (uint16_t i = 0; i < MDNS_NAME_LEN; i++) {
        uint8_t a = qname[i], b = mdns_name_wled[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static void mdns_send_response(struct udp_pcb *pcb, const ip_addr_t *dst,
                                u16_t port, uint16_t txn_id) {
    /*
     * Response layout:
     *   DNS header (12 bytes): 1 answer, 0 questions
     *   Answer: wled.local. A IN 169.254.7.1, TTL=120
     */
    uint8_t resp[12 + MDNS_NAME_LEN + 10 + 4];
    memset(resp, 0, sizeof(resp));

    /* DNS header */
    resp[0] = (uint8_t)(txn_id >> 8);
    resp[1] = (uint8_t)(txn_id);
    resp[2] = 0x84;  /* QR=1, AA=1 */
    resp[3] = 0x00;
    /* QDCOUNT=0, ANCOUNT=1, NSCOUNT=0, ARCOUNT=0 */
    resp[7] = 1;

    /* Answer section */
    uint16_t off = 12;
    memcpy(&resp[off], mdns_name_wled, MDNS_NAME_LEN);
    off += MDNS_NAME_LEN;

    /* Type A (1), class IN (1) with cache-flush bit */
    resp[off++] = 0x00; resp[off++] = 0x01;  /* TYPE A */
    resp[off++] = 0x80; resp[off++] = 0x01;  /* CLASS IN + cache-flush */

    /* TTL */
    resp[off++] = 0x00; resp[off++] = 0x00;
    resp[off++] = (MDNS_TTL >> 8) & 0xFF;
    resp[off++] = MDNS_TTL & 0xFF;

    /* RDLENGTH = 4 (IPv4 address) */
    resp[off++] = 0x00; resp[off++] = 0x04;

    /* RDATA: 169.254.7.1 */
    resp[off++] = 169; resp[off++] = 254;
    resp[off++] = 7;   resp[off++] = 1;

    struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, off, PBUF_RAM);
    if (!rp) return;
    memcpy(rp->payload, resp, off);

    /* Send to mDNS multicast address */
    ip_addr_t mdns_addr = IPADDR4_INIT_BYTES(224, 0, 0, 251);
    udp_sendto_if(pcb, rp, &mdns_addr, MDNS_PORT, &usb_netif);
    pbuf_free(rp);
}

static void mdns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *addr, u16_t port) {
    (void)arg;

    if (p->tot_len < 12) { pbuf_free(p); return; }

    uint8_t hdr[12];
    pbuf_copy_partial(p, hdr, 12, 0);

    /* Only process queries (QR=0) */
    if (hdr[2] & 0x80) { pbuf_free(p); return; }

    uint16_t txn_id = (hdr[0] << 8) | hdr[1];
    uint16_t qdcount = (hdr[4] << 8) | hdr[5];

    /* Parse questions looking for wled.local A/ANY */
    uint16_t off = 12;
    for (uint16_t q = 0; q < qdcount; q++) {
        /* Read QNAME */
        uint8_t qname[64];
        uint16_t qi = 0;
        while (off < p->tot_len && qi < sizeof(qname) - 1) {
            uint8_t byte;
            pbuf_copy_partial(p, &byte, 1, off);
            qname[qi++] = byte;
            if (byte == 0) { off++; break; }
            off++;
            if (byte >= 0xC0) { off++; qi++; break; }  /* compression ptr */
        }
        if (off + 4 > p->tot_len) break;

        uint8_t qtype_class[4];
        pbuf_copy_partial(p, qtype_class, 4, off);
        off += 4;

        uint16_t qtype = (qtype_class[0] << 8) | qtype_class[1];
        /* A=1, AAAA=28, ANY=255 */
        if ((qtype == 1 || qtype == 255) && mdns_name_match(qname, qi)) {
            mdns_send_response(pcb, addr, port, txn_id);
            break;
        }
    }
    pbuf_free(p);
}

static void mdns_responder_init(void) {
    mdns_pcb = udp_new();
    if (!mdns_pcb) return;

    ip_addr_t any;
    ip_addr_set_any(false, &any);
    udp_bind(mdns_pcb, &any, MDNS_PORT);
    udp_recv(mdns_pcb, mdns_recv, NULL);

    /* Join mDNS multicast group on USB netif */
    ip4_addr_t mdns_group;
    IP4_ADDR(&mdns_group, 224, 0, 0, 251);
    igmp_joingroup(netif_ip4_addr(&usb_netif), &mdns_group);
}

/* Send unsolicited mDNS announcement (call after PPP up / periodically) */
static void mdns_announce(void) {
    if (!mdns_pcb) return;
    mdns_send_response(mdns_pcb, NULL, MDNS_PORT, 0);
}

/* ------------------------------------------------------------------ */
/* USB NCM netif                                                       */
/* ------------------------------------------------------------------ */

static err_t usb_linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void)netif;
    for (;;) {
        if (!tud_ready()) return ERR_USE;
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        tud_task();
    }
}

static err_t usb_ip4_output_fn(struct netif *netif, struct pbuf *p,
                                const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

#if LWIP_IPV6
static err_t usb_ip6_output_fn(struct netif *netif, struct pbuf *p,
                                const ip6_addr_t *addr) {
    return ethip6_output(netif, p, addr);
}
#endif

static err_t usb_netif_init_cb(struct netif *netif) {
    netif->mtu        = CFG_TUD_NET_MTU;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                        NETIF_FLAG_LINK_UP | NETIF_FLAG_UP |
                        NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    netif->state      = NULL;
    netif->name[0]    = 'U';
    netif->name[1]    = 'S';
    netif->linkoutput = usb_linkoutput_fn;
    netif->output     = usb_ip4_output_fn;
#if LWIP_IPV6
    netif->output_ip6 = usb_ip6_output_fn;
#endif
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* TinyUSB network callbacks                                           */
/* ------------------------------------------------------------------ */

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (received_frame) return false;
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            memcpy(p->payload, src, size);
            received_frame = p;
        }
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    struct pbuf *p = (struct pbuf *)ref;
    (void)arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void) {
    /* Reset state on host re-enumeration */
}

/* ------------------------------------------------------------------ */
/* UART RX interrupt                                                   */
/* ------------------------------------------------------------------ */

static void uart_rx_irq_handler(void) {
    while (uart_is_readable(BRIDGE_UART_ID)) {
        uint8_t ch = uart_getc(BRIDGE_UART_ID);
        uint32_t next = (uart_rx_head + 1) % UART_RX_BUF_SIZE;
        if (next != uart_rx_tail) {
            uart_rx_buf[uart_rx_head] = ch;
            uart_rx_head = next;
        }
    }
}

static void uart_bridge_init(void) {
    uart_init(BRIDGE_UART_ID, BRIDGE_UART_BAUD);
    gpio_set_function(BRIDGE_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BRIDGE_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(BRIDGE_UART_ID, false, false);
    uart_set_format(BRIDGE_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(BRIDGE_UART_ID, true);

    int uart_irq = (BRIDGE_UART_ID == uart0) ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(uart_irq, uart_rx_irq_handler);
    irq_set_enabled(uart_irq, true);
    uart_set_irq_enables(BRIDGE_UART_ID, true, false);

    uart_rx_head = 0;
    uart_rx_tail = 0;
}

/* ------------------------------------------------------------------ */
/* PPP callbacks                                                       */
/* ------------------------------------------------------------------ */

static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len,
                            void *ctx) {
    (void)pcb; (void)ctx;
    const uint8_t *buf = (const uint8_t *)data;
    for (u32_t i = 0; i < len; i++) {
        uart_putc_raw(BRIDGE_UART_ID, buf[i]);
    }
    return len;
}

static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
    (void)ctx;
    struct netif *pppif = ppp_netif(pcb);

    switch (err_code) {
        case PPPERR_NONE:
            ppp_connected = true;
            printf("PPP up: local=%s", ip4addr_ntoa(netif_ip4_addr(pppif)));
            printf(" peer=%s\n", ip4addr_ntoa(netif_ip4_gw(pppif)));
            /* Announce ESP32 reachability to host */
            send_gratuitous_arp();
            mdns_announce();
            break;
        case PPPERR_USER:
            ppp_connected = false;
            ppp_free(pcb);
            break;
        default:
            ppp_connected = false;
            printf("PPP err %d, reconnecting\n", err_code);
            ppp_connect(pcb, 0);
            break;
    }
}

static void ppp_bridge_init(void) {
    ppp_pcb_inst = pppos_create(&ppp_netif_data, ppp_output_cb,
                                ppp_status_cb, NULL);
    if (!ppp_pcb_inst) {
        printf("pppos_create failed!\n");
        return;
    }
    ppp_connect(ppp_pcb_inst, 0);
}

/* ------------------------------------------------------------------ */
/* Main loop helpers                                                   */
/* ------------------------------------------------------------------ */

static void process_uart_rx(void) {
    while (uart_rx_tail != uart_rx_head) {
        uint8_t byte = uart_rx_buf[uart_rx_tail];
        uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;
        pppos_input(ppp_pcb_inst, &byte, 1);
    }
}

static void process_usb_rx(void) {
    if (received_frame) {
        struct pbuf *p = received_frame;
        received_frame = NULL;
        tud_network_recv_renew();

        /* Proxy ARP: intercept before lwIP so host can reach ESP32 */
        handle_proxy_arp(p);

        /* Feed raw Ethernet frame through proper Ethernet+ARP processing */
        if (ethernet_input(p, &usb_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}

static void led_blink_task(void) {
    static uint32_t last_ms;
    static bool led_state;
    uint32_t now = board_millis();
    if (now - last_ms < BLINK_INTERVAL_MS) return;
    last_ms = now;
    board_led_write(led_state);
    led_state = !led_state;
}

/* Periodic mDNS re-announcement (every 60s) */
static void mdns_periodic_task(void) {
    static uint32_t last_ms;
    uint32_t now = board_millis();
    if (now - last_ms < 60000) return;
    last_ms = now;
    if (ppp_connected) mdns_announce();
}

/* ------------------------------------------------------------------ */
/* lwIP + main                                                         */
/* ------------------------------------------------------------------ */

static void init_lwip(void) {
    ip4_addr_t ipaddr, netmask, gw;
    lwip_init();

    ipaddr.addr  = PP_HTONL(USB_IP_ADDR);
    netmask.addr = PP_HTONL(USB_NETMASK);
    gw.addr      = PP_HTONL(USB_GW);

    usb_netif.hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(usb_netif.hwaddr, tud_network_mac_address,
           sizeof(tud_network_mac_address));
    usb_netif.hwaddr[5] ^= 0x01;

    netif_add(&usb_netif, &ipaddr, &netmask, &gw, NULL,
              usb_netif_init_cb, ethernet_input);
    netif_set_default(&usb_netif);

#if LWIP_IPV6
    /* Assign fe80::3 as static IPv6 link-local on USB netif */
    ip6_addr_t usb_ip6;
    IP6_ADDR(&usb_ip6, PP_HTONL(0xfe800000UL), 0, 0, PP_HTONL(0x3UL));
    netif_ip6_addr_set(&usb_netif, 0, &usb_ip6);
    netif_ip6_addr_set_state(&usb_netif, 0, IP6_ADDR_VALID);
#endif
}

int main(void) {
    board_init();
    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    init_lwip();
    dhcp_server_init();
    mdns_responder_init();
    uart_bridge_init();
    ppp_bridge_init();

    printf("WLED USB Bridge started\n");
    printf("USB: 169.254.7.3/24, DHCP: 169.254.7.2\n");
    printf("ESP32: 169.254.7.1 (wled.local via mDNS)\n");
    printf("UART%d: %d baud (GP%d TX, GP%d RX)\n",
           (BRIDGE_UART_ID == uart0) ? 0 : 1,
           BRIDGE_UART_BAUD, BRIDGE_UART_TX_PIN, BRIDGE_UART_RX_PIN);

    while (1) {
        tud_task();
        process_usb_rx();
        process_uart_rx();
        sys_check_timeouts();
        led_blink_task();
        mdns_periodic_task();
    }
    return 0;
}
