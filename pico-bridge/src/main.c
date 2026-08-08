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
 *   - usb_netif:  192.168.7.1/24  (Ethernet, faces host)
 *   - ppp_netif:  negotiated      (PPP client, faces ESP32)
 *
 * The host gets 192.168.7.2 via built-in DHCP server.
 * Traffic to the ESP32 PPP peer is forwarded by lwIP.
 *
 * UART1 pins (configurable):
 *   GP4 = TX -> ESP32 RX (G33 on M5StickC)
 *   GP5 = RX <- ESP32 TX (G32 on M5StickC)
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
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/ip.h"
#include "lwip/igmp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
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

/* USB-side IP addressing */
#define USB_IP_ADDR      LWIP_MAKEU32(192, 168, 7, 1)
#define USB_NETMASK      LWIP_MAKEU32(255, 255, 255, 0)
#define USB_GW           LWIP_MAKEU32(0, 0, 0, 0)

/* DHCP pool */
#define DHCP_POOL_START  LWIP_MAKEU32(192, 168, 7, 2)
#define DHCP_POOL_GW     LWIP_MAKEU32(192, 168, 7, 1)
#define DHCP_SERVER_PORT 67

/* UART RX ring buffer */
#define UART_RX_BUF_SIZE 4096

/* LED blink */
#define BLINK_INTERVAL_MS 500

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
/* Minimal DHCP Server (single-client)                                 */
/* ------------------------------------------------------------------ */

#define DHCP_OP_REQUEST   1
#define DHCP_OP_REPLY     2
#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SRV_ID   54
#define DHCP_OPT_LEASE    51
#define DHCP_OPT_SUBNET   1
#define DHCP_OPT_ROUTER   3
#define DHCP_OPT_DNS      6
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
    reply.yiaddr = PP_HTONL(DHCP_POOL_START);
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
    dhcp_add_opt_u32(reply.options, &oi, DHCP_OPT_ROUTER, DHCP_POOL_GW);
    dhcp_add_opt_u32(reply.options, &oi, DHCP_OPT_DNS, USB_IP_ADDR);
    reply.options[oi++] = DHCP_OPT_END;

    struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, sizeof(reply), PBUF_RAM);
    if (rp) {
        memcpy(rp->payload, &reply, sizeof(reply));
        ip_addr_t bcast;
        IP4_ADDR(&bcast, 255, 255, 255, 255);
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

static err_t usb_netif_init_cb(struct netif *netif) {
    netif->mtu        = CFG_TUD_NET_MTU;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                        NETIF_FLAG_LINK_UP | NETIF_FLAG_UP |
                        NETIF_FLAG_IGMP;
    netif->state      = NULL;
    netif->name[0]    = 'U';
    netif->name[1]    = 'S';
    netif->linkoutput = usb_linkoutput_fn;
    netif->output     = usb_ip4_output_fn;
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
            printf("PPP up: %s\n", ip4addr_ntoa(netif_ip4_addr(pppif)));
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
    /* No authentication needed (PAP/CHAP disabled in lwipopts.h) */
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
        if (usb_netif.input(p, &usb_netif) != ERR_OK) {
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
              usb_netif_init_cb, ip_input);
    netif_set_default(&usb_netif);
}

int main(void) {
    board_init();
    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    init_lwip();
    dhcp_server_init();
    uart_bridge_init();
    ppp_bridge_init();

    printf("WLED USB Bridge started\n");
    printf("USB: 192.168.7.1/24, DHCP: 192.168.7.2\n");
    printf("UART%d: %d baud (GP%d TX, GP%d RX)\n",
           (BRIDGE_UART_ID == uart0) ? 0 : 1,
           BRIDGE_UART_BAUD, BRIDGE_UART_TX_PIN, BRIDGE_UART_RX_PIN);

    while (1) {
        tud_task();
        process_usb_rx();
        process_uart_rx();
        sys_check_timeouts();
        led_blink_task();
    }
    return 0;
}
