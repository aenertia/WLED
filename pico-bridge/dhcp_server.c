/*
 * Minimal DHCP server  -- serves exactly ONE client on the NCM interface.
 * Handles DISCOVER ->OFFER and REQUEST ->ACK. No lease tracking, no NAK.
 * Good enough for a single-host USB Ethernet bridge.
 */
#include "dhcp_server.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/dhcp.h"
#include <string.h>

/* DHCP message offsets (RFC 2131) */
#define DHCP_OP_REPLY       2
#define DHCP_HTYPE_ETH      1
#define DHCP_HLEN_ETH       6
/* DHCP_MAGIC_COOKIE already defined in lwip/prot/dhcp.h */

/* DHCP option codes */
#define DHCP_OPT_MSG_TYPE   53
#define DHCP_OPT_SERVER_ID  54
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_END        255

/* DHCP message types */
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_ACK            5

/* Minimal DHCP message structure (240 bytes fixed + options) */
typedef struct __attribute__((packed)) {
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
    uint8_t  options[312];  /* Max DHCP options */
} dhcp_msg_t;

static struct udp_pcb *dhcp_pcb;
static struct netif *dhcp_netif;
static ip4_addr_t dhcp_client_ip;

static const uint8_t *find_option(const uint8_t *opts, size_t len, uint8_t code) {
    size_t i = 0;
    while (i < len) {
        if (opts[i] == DHCP_OPT_END) return NULL;
        if (opts[i] == 0) { i++; continue; }  /* pad */
        if (opts[i] == code) return &opts[i];
        if (i + 1 >= len) return NULL;
        i += 2 + opts[i + 1];
    }
    return NULL;
}

static void send_reply(const dhcp_msg_t *req, uint8_t msg_type) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_msg_t), PBUF_RAM);
    if (!p) return;

    dhcp_msg_t *reply = (dhcp_msg_t *)p->payload;
    memset(reply, 0, sizeof(dhcp_msg_t));

    reply->op     = DHCP_OP_REPLY;
    reply->htype  = DHCP_HTYPE_ETH;
    reply->hlen   = DHCP_HLEN_ETH;
    reply->xid    = req->xid;
    reply->yiaddr = dhcp_client_ip.addr;
    reply->siaddr = ip4_addr_get_u32(netif_ip4_addr(dhcp_netif));
    memcpy(reply->chaddr, req->chaddr, 16);
    reply->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);

    uint8_t *opt = reply->options;
    int oi = 0;

    opt[oi++] = DHCP_OPT_MSG_TYPE;
    opt[oi++] = 1;
    opt[oi++] = msg_type;

    opt[oi++] = DHCP_OPT_SERVER_ID;
    opt[oi++] = 4;
    uint32_t server_ip = ip4_addr_get_u32(netif_ip4_addr(dhcp_netif));
    memcpy(&opt[oi], &server_ip, 4);
    oi += 4;

    /* Lease time (1 day  -- doesn't really matter, device is always on) */
    opt[oi++] = DHCP_OPT_LEASE_TIME;
    opt[oi++] = 4;
    uint32_t lease = PP_HTONL(86400);
    memcpy(&opt[oi], &lease, 4);
    oi += 4;

    opt[oi++] = DHCP_OPT_SUBNET;
    opt[oi++] = 4;
    uint32_t mask = PP_HTONL(0xFFFFFF00);  /* /24 */
    memcpy(&opt[oi], &mask, 4);
    oi += 4;

    /* Router (gateway = us) */
    opt[oi++] = DHCP_OPT_ROUTER;
    opt[oi++] = 4;
    memcpy(&opt[oi], &server_ip, 4);
    oi += 4;

    opt[oi++] = DHCP_OPT_END;

    pbuf_realloc(p, (uint16_t)(240 + 4 + oi));  /* fixed fields + cookie + options */

    /* Send as broadcast (client doesn't have an IP yet) */
    ip_addr_t dst;
    ip_addr_set_ip4_u32_val(dst, IPADDR_BROADCAST);
    udp_sendto_if(dhcp_pcb, p, &dst, 68, dhcp_netif);

    pbuf_free(p);
}

static void dhcp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;

    if (p->tot_len < 240 + 4) {  /* Too short for DHCP */
        pbuf_free(p);
        return;
    }

    dhcp_msg_t msg;
    pbuf_copy_partial(p, &msg, sizeof(msg), 0);
    pbuf_free(p);

    if (msg.op != 1 || msg.htype != DHCP_HTYPE_ETH ||
        PP_NTOHL(msg.cookie) != DHCP_MAGIC_COOKIE) {
        return;
    }

    size_t opts_len = sizeof(msg.options);
    const uint8_t *mt = find_option(msg.options, opts_len, DHCP_OPT_MSG_TYPE);
    if (!mt || mt[1] < 1) return;

    uint8_t msg_type = mt[2];

    if (msg_type == DHCP_DISCOVER) {
        send_reply(&msg, DHCP_OFFER);
    } else if (msg_type == DHCP_REQUEST) {
        send_reply(&msg, DHCP_ACK);
    }
    /* Ignore RELEASE, INFORM, DECLINE  -- we don't care */
}

void dhcp_server_init(struct netif *netif, const ip4_addr_t *client_ip) {
    dhcp_netif = netif;
    dhcp_client_ip = *client_ip;

    dhcp_pcb = udp_new();
    if (!dhcp_pcb) return;

    udp_bind(dhcp_pcb, IP4_ADDR_ANY, 67);
    udp_recv(dhcp_pcb, dhcp_recv_cb, NULL);
}

void dhcp_server_deinit(void) {
    if (dhcp_pcb) {
        udp_remove(dhcp_pcb);
        dhcp_pcb = NULL;
    }
}
