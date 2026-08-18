/*
 * Pi Pico NCM-to-PPP NAT Bridge
 *
 * Host PC <--USB CDC-NCM--> Pico <--UART PPP 5Mbps--> ESP32 M5StickC
 *
 * The Pico presents as a USB Ethernet adapter (CDC-NCM) to the host.
 * It connects to the ESP32's PPP server over UART GP0(TX)/GP1(RX).
 * NAT relay forwards TCP/UDP between the two interfaces:
 *   - TCP :80  → ESP32:80  (HTTP, WebSocket — WLED dashboard)
 *   - UDP :4048 → ESP32:4048 (DDP pixel streaming)
 *   - UDP :5568 → ESP32:5568 (E1.31/sACN)
 *
 * The host sees a plug-and-play USB Ethernet adapter. No pppd needed.
 * DHCP assigns 10.0.0.2/24 to the host, gateway 10.0.0.1 (the Pico).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#include "bsp/board_api.h"
#include "tusb.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "netif/ethernet.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"

#include "dhcp_server.h"
#include "nat_relay.h"

/* ---- Configuration ---- */
#define PPP_UART        uart0
#define PPP_UART_IRQ    UART0_IRQ
#define PPP_TX_PIN      0
#define PPP_RX_PIN      1
#define PPP_BAUD        5000000

/* NCM-side IP addressing */
#define NCM_IP          LWIP_MAKEU32(10, 0, 0, 1)
#define NCM_MASK        LWIP_MAKEU32(255, 255, 255, 0)
#define NCM_CLIENT_IP   LWIP_MAKEU32(10, 0, 0, 2)

/* Forwarded ports */
#define PORT_HTTP       80
#define PORT_DDP        4048
#define PORT_E131       5568

/* UART RX ring buffer — sized for 5Mbps burst absorption */
#define RX_BUF_SIZE     4096
static volatile uint8_t rx_buf[RX_BUF_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;

/* ---- Global state ---- */
static struct netif ncm_netif;
static struct netif ppp_netif;
static ppp_pcb *ppp;
static bool ppp_connected = false;
static bool ncm_link_up = false;

/* LED blink for status (Pico onboard LED) */
#define LED_PIN         PICO_DEFAULT_LED_PIN

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif

static volatile uint8_t mdns_announce_remaining;
static uint32_t mdns_announce_next_ms;

/* ---- Forward declarations ---- */
extern void usb_descriptors_init(void);
extern uint8_t tud_network_mac_address[6];

/* ======================================================================
 * UART RX — interrupt-driven ring buffer
 * At 5Mbps, byte time is 2µs. FIFO is 32 bytes = 64µs to drain.
 * IRQ latency on RP2040 is ~1µs, so IRQ-based RX is viable with FIFO.
 * ====================================================================== */

static void uart_rx_irq(void) {
    while (uart_is_readable(PPP_UART)) {
        uint8_t ch = uart_getc(PPP_UART);
        uint16_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = ch;
            rx_head = next;
        }
        /* else: overflow — drop byte. PPP HDLC CRC will catch it. */
    }
}

/* Drain ring buffer → pppos_input(). Called from main loop only. */
static void ppp_poll_rx(void) {
    uint8_t tmp[512];
    int n = 0;

    /* Snapshot head to avoid race with IRQ */
    uint16_t head = rx_head;
    while (rx_tail != head && n < (int)sizeof(tmp)) {
        tmp[n++] = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    }
    if (n > 0 && ppp) {
        pppos_input(ppp, tmp, n);
    }
}

/* ======================================================================
 * PPP — client connecting to ESP32 server
 * ====================================================================== */

/* PPPoS output callback — lwIP calls this to send HDLC frames */
static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
    (void)pcb; (void)ctx;
    uart_write_blocking(PPP_UART, (const uint8_t *)data, len);
    return len;
}

/* PPP link status callback */
static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
    (void)ctx;

    switch (err_code) {
    case PPPERR_NONE: {
        const ip4_addr_t *peer_ip = netif_ip4_gw(&ppp_netif);

        ppp_connected = true;
        nat_relay_set_peer(peer_ip);
        nat_relay_set_ppp_up(true);

        mdns_announce_remaining = 3;
        mdns_announce_next_ms = sys_now();

        gpio_put(LED_PIN, 1);
        break;
    }
    case PPPERR_USER:
        /* Intentional close */
        ppp_connected = false;
        nat_relay_set_ppp_up(false);
        gpio_put(LED_PIN, 0);
        break;
    default:
        /* Link lost — reconnect after 1s holdoff */
        ppp_connected = false;
        nat_relay_set_ppp_up(false);
        gpio_put(LED_PIN, 0);
        ppp_connect(pcb, 1);
        break;
    }
}

static void ppp_init_client(void) {
    ppp = pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, NULL);
    if (!ppp) {
        /* Fatal — can't continue */
        while (1) { tight_loop_contents(); }
    }

    /* Accept whatever IPs the ESP32 server assigns */
    ppp_set_default(ppp);

    /* Start LCP negotiation */
    ppp_connect(ppp, 0);
}

/* ======================================================================
 * CDC-NCM — TinyUSB network callbacks
 * ====================================================================== */

static struct netif *ncm_netif_ptr = NULL;

/* Receive callback: host → Pico (USB RX → lwIP) */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (!ncm_netif_ptr) {
        tud_network_recv_renew();
        return true;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) {
        tud_network_recv_renew();
        return false;
    }

    pbuf_take(p, src, size);

    if (ncm_netif_ptr->input(p, ncm_netif_ptr) != ERR_OK) {
        pbuf_free(p);
    }

    tud_network_recv_renew();
    return true;
}

/* Transmit callback: lwIP pbuf → USB TX buffer */
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    struct pbuf *p = (struct pbuf *)ref;
    (void)arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

/* NCM init callback (ECM/RNDIS only, not used for NCM) */
void tud_network_init_cb(void) {
    /* Nothing to do for NCM */
}

/* Link output: lwIP → USB TX */
static err_t ncm_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;

    if (!tud_ready()) return ERR_USE;

    /* Spin until we can transmit (TinyUSB processes in tud_task) */
    for (int i = 0; i < 100; i++) {
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        tud_task();
    }
    return ERR_USE;
}

/* NCM netif init */
static err_t ncm_netif_init(struct netif *netif) {
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                 | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP
                 | NETIF_FLAG_IGMP;
    netif->state = NULL;
    netif->name[0] = 'n';
    netif->name[1] = '0';
    netif->linkoutput = ncm_linkoutput;
    netif->output = etharp_output;
    return ERR_OK;
}

/* Set up the NCM lwIP netif */
static void ncm_lwip_init(void) {
    ip4_addr_t ipaddr, netmask, gw;

    IP4_ADDR(&ipaddr,  10, 0, 0, 1);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      0, 0, 0, 0);

    struct netif *nif = netif_add(&ncm_netif, &ipaddr, &netmask, &gw,
                                  NULL, ncm_netif_init, ethernet_input);
    if (!nif) {
        while (1) { tight_loop_contents(); }
    }

    /* Set MAC — device MAC differs from host MAC (toggle LSbit per TinyUSB convention) */
    ncm_netif.hwaddr_len = 6;
    memcpy(ncm_netif.hwaddr, tud_network_mac_address, 6);
    ncm_netif.hwaddr[5] ^= 0x01;

    netif_set_up(&ncm_netif);
    netif_set_link_up(&ncm_netif);

    ncm_netif_ptr = &ncm_netif;
}

/* ======================================================================
 * mDNS responder — announces wled.local → Pico NCM IP (10.0.0.1)
 * Host connects to Pico; NAT relay forwards to ESP32 transparently.
 * ====================================================================== */

#define MDNS_PORT   5353
#define MDNS_TTL    120

static const uint8_t mdns_name_wled[] = {4,'w','l','e','d', 5,'l','o','c','a','l', 0};
#define MDNS_NAME_LEN sizeof(mdns_name_wled)

static struct udp_pcb *mdns_pcb;

static bool mdns_name_match(const uint8_t *qname, uint16_t qlen) {
    if (qlen < MDNS_NAME_LEN) return false;
    for (uint16_t i = 0; i < MDNS_NAME_LEN; i++) {
        uint8_t a = qname[i], b = mdns_name_wled[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return false;
    }
    return true;
}

static void mdns_send_response(uint16_t txn_id) {
    uint8_t resp[12 + MDNS_NAME_LEN + 10 + 4];
    memset(resp, 0, sizeof(resp));

    resp[0] = (uint8_t)(txn_id >> 8);
    resp[1] = (uint8_t)(txn_id);
    resp[2] = 0x84;  /* QR=1, AA=1 */
    resp[7] = 1;     /* ANCOUNT=1 */

    uint16_t off = 12;
    memcpy(&resp[off], mdns_name_wled, MDNS_NAME_LEN);
    off += MDNS_NAME_LEN;

    resp[off++] = 0x00; resp[off++] = 0x01;  /* TYPE A */
    resp[off++] = 0x80; resp[off++] = 0x01;  /* CLASS IN + cache-flush */
    resp[off++] = 0x00; resp[off++] = 0x00;
    resp[off++] = (MDNS_TTL >> 8) & 0xFF;
    resp[off++] = MDNS_TTL & 0xFF;
    resp[off++] = 0x00; resp[off++] = 0x04;

    /* Pico NCM IP: 10.0.0.1 — host connects here, NAT forwards to ESP32 */
    resp[off++] = 10; resp[off++] = 0;
    resp[off++] = 0;  resp[off++] = 1;

    struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, off, PBUF_RAM);
    if (!rp) return;
    memcpy(rp->payload, resp, off);

    ip_addr_t mdns_addr = IPADDR4_INIT_BYTES(224, 0, 0, 251);
    udp_sendto_if(mdns_pcb, rp, &mdns_addr, MDNS_PORT, &ncm_netif);
    pbuf_free(rp);
}

static void mdns_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;

    if (p->tot_len < 12) { pbuf_free(p); return; }

    uint8_t hdr[12];
    pbuf_copy_partial(p, hdr, 12, 0);
    if (hdr[2] & 0x80) { pbuf_free(p); return; }  /* skip responses */

    uint16_t txn_id = (hdr[0] << 8) | hdr[1];
    uint16_t qdcount = (hdr[4] << 8) | hdr[5];

    uint16_t off = 12;
    for (uint16_t q = 0; q < qdcount; q++) {
        uint8_t qname[64];
        uint16_t qi = 0;
        while (off < p->tot_len && qi < sizeof(qname) - 1) {
            uint8_t byte;
            pbuf_copy_partial(p, &byte, 1, off++);
            qname[qi++] = byte;
            if (byte == 0) break;
            if (byte >= 0xC0) { off++; qi++; break; }
        }
        if (off + 4 > p->tot_len) break;

        uint8_t qt[4];
        pbuf_copy_partial(p, qt, 4, off);
        off += 4;

        uint16_t qtype = (qt[0] << 8) | qt[1];
        if ((qtype == 1 || qtype == 255) && mdns_name_match(qname, qi)) {
            mdns_send_response(txn_id);
            break;
        }
    }
    pbuf_free(p);
}

static void mdns_init(void) {
    mdns_pcb = udp_new();
    if (!mdns_pcb) return;

    ip_addr_t any;
    ip_addr_set_any(false, &any);
    udp_bind(mdns_pcb, &any, MDNS_PORT);
    udp_recv(mdns_pcb, mdns_recv_cb, NULL);

    ip4_addr_t mdns_group;
    IP4_ADDR(&mdns_group, 224, 0, 0, 251);
    igmp_joingroup(netif_ip4_addr(&ncm_netif), &mdns_group);
}

static void mdns_announce_burst_task(void) {
    if (mdns_announce_remaining == 0) return;
    uint32_t now = sys_now();
    if ((int32_t)(now - mdns_announce_next_ms) < 0) return;

    mdns_send_response(0);
    mdns_announce_remaining--;
    mdns_announce_next_ms = now + 1000;
}

static void mdns_periodic_task(void) {
    static uint32_t last_ms;
    uint32_t now = sys_now();
    if (now - last_ms < 60000) return;
    last_ms = now;
    if (ppp_connected) mdns_send_response(0);
}

/* ======================================================================
 * UART hardware setup
 * ====================================================================== */

static void uart_hw_init(void) {
    /* Configure GPIO pins for UART */
    gpio_set_function(PPP_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PPP_RX_PIN, GPIO_FUNC_UART);

    /* Init UART at 5Mbps */
    uint actual = uart_init(PPP_UART, PPP_BAUD);
    (void)actual;

    /* Enable FIFO for burst absorption */
    uart_set_fifo_enabled(PPP_UART, true);

    /* No flow control */
    uart_set_hw_flow(PPP_UART, false, false);
    uart_set_format(PPP_UART, 8, 1, UART_PARITY_NONE);

    /* Drive strength for 5Mbps signal integrity */
    gpio_set_drive_strength(PPP_TX_PIN, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_slew_rate(PPP_TX_PIN, GPIO_SLEW_RATE_FAST);

    /* Enable RX interrupt */
    irq_set_exclusive_handler(PPP_UART_IRQ, uart_rx_irq);
    irq_set_enabled(PPP_UART_IRQ, true);
    uart_set_irq_enables(PPP_UART, true, false);
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    /* Basic hardware init (clocks, etc.) */
    set_sys_clock_khz(125000, true);

    /* LED for status */
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    /* Generate stable MAC from board unique ID */
    usb_descriptors_init();

    /* Initialize TinyUSB */
    board_init();
    tud_init(BOARD_TUD_RHPORT);

    /* Initialize lwIP (bare-metal) */
    lwip_init();

    /* Set up NCM network interface */
    ncm_lwip_init();

    /* Start DHCP server on NCM */
    ip4_addr_t client_ip;
    IP4_ADDR(&client_ip, 10, 0, 0, 2);
    dhcp_server_init(&ncm_netif, &client_ip);

    /* Initialize NAT relay (listeners start immediately) */
    nat_relay_init(&ncm_netif);
    nat_relay_add_tcp(PORT_HTTP);
    nat_relay_add_udp(PORT_DDP);
    nat_relay_add_udp(PORT_E131);

    /* mDNS: wled.local → 10.0.0.1 (Pico NCM IP, NATed to ESP32) */
    mdns_init();

    /* Set up UART for PPP */
    uart_hw_init();

    /* Start PPP client — connects to ESP32 server */
    ppp_init_client();

    /* ---- Main loop ---- */
    uint32_t last_blink = 0;

    while (1) {
        /* TinyUSB device task — processes USB events */
        tud_task();

        /* Feed UART RX bytes into PPP stack */
        ppp_poll_rx();

        /* lwIP timers (ARP, TCP retransmit, PPP LCP/IPCP, ...) */
        sys_check_timeouts();

        /* mDNS announcement burst (3x at 1s intervals on PPP link-up) */
        mdns_announce_burst_task();
        mdns_periodic_task();

        /* Status LED blink when PPP is negotiating */
        if (!ppp_connected) {
            uint32_t now = sys_now();
            if (now - last_blink > 250) {
                gpio_xor_mask(1u << LED_PIN);
                last_blink = now;
            }
        }
    }

    return 0;
}
