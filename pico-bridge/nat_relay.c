/*
 * NAT relay  -- TCP proxy + UDP relay between NCM and PPP interfaces.
 *
 * TCP: Full bidirectional proxy with backpressure handling.
 * UDP: Stateless forwarding with source tracking for return path.
 * DDP (port 4048): Transparent compression before forwarding over PPP.
 *
 * All callbacks run from the lwIP main loop (NO_SYS=1).
 */
#include "nat_relay.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "ddp_compress.h"
#include <string.h>
#include <stdio.h>

static struct netif *ncm_nif;
static ip4_addr_t peer_ip;
static bool ppp_up = false;

/* DDP compression state (port 4048) */
static uint8_t  ddp_prev_frame[DDP_CHANNELS_PER_PACKET];
static uint8_t  ddp_comp_buf[DDP_CHANNELS_PER_PACKET + 16];
static uint8_t  ddp_workspace[DDP_CHANNELS_PER_PACKET * 2 + 16];
static uint16_t ddp_frame_count = 0;
static bool     ddp_has_prev = false;

/* TCP relay */

typedef struct tcp_relay {
    struct tcp_pcb *client;     /* NCM-side (from host) */
    struct tcp_pcb *server;     /* PPP-side (to ESP32) */
    uint16_t port;
    bool client_closed;
    bool server_closed;
    struct tcp_relay *next;
} tcp_relay_t;

typedef struct tcp_listener {
    struct tcp_pcb *listen_pcb;
    uint16_t port;
    struct tcp_listener *next;
} tcp_listener_t;

static tcp_relay_t *active_relays = NULL;
static tcp_listener_t *tcp_listeners = NULL;

static void relay_cleanup(tcp_relay_t *r);

static err_t relay_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t relay_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void relay_client_err(void *arg, err_t err);
static void relay_server_err(void *arg, err_t err);
static err_t relay_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t relay_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);

static void relay_cleanup(tcp_relay_t *r) {
    tcp_relay_t **pp = &active_relays;
    while (*pp && *pp != r) pp = &(*pp)->next;
    if (*pp) *pp = r->next;

    if (r->client) {
        tcp_arg(r->client, NULL);
        tcp_recv(r->client, NULL);
        tcp_err(r->client, NULL);
        tcp_sent(r->client, NULL);
        tcp_abort(r->client);
        r->client = NULL;
    }
    if (r->server) {
        tcp_arg(r->server, NULL);
        tcp_recv(r->server, NULL);
        tcp_err(r->server, NULL);
        tcp_sent(r->server, NULL);
        tcp_abort(r->server);
        r->server = NULL;
    }
    free(r);
}

/* Client (host) sent data  -> forward to server (ESP32) */
static err_t relay_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    tcp_relay_t *r = (tcp_relay_t *)arg;
    (void)tpcb; (void)err;

    if (!p) {
        /* Client closed connection */
        r->client_closed = true;
        if (r->server && !r->server_closed) {
            tcp_shutdown(r->server, 0, 1);  /* half-close server TX */
        }
        if (r->server_closed) relay_cleanup(r);
        return ERR_OK;
    }

    if (!r->server || r->server_closed) {
        pbuf_free(p);
        return ERR_OK;
    }

    /* Write to server, handling backpressure */
    err_t werr = tcp_write(r->server, p->payload, p->tot_len, TCP_WRITE_FLAG_COPY);
    if (werr == ERR_OK) {
        tcp_recved(r->client, p->tot_len);
        tcp_output(r->server);
    } else if (werr == ERR_MEM) {
        /* Backpressure: don't ack, lwIP will retry */
        pbuf_free(p);
        return ERR_MEM;
    }

    pbuf_free(p);
    return ERR_OK;
}

/* Server (ESP32) sent data  -> forward to client (host) */
static err_t relay_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    tcp_relay_t *r = (tcp_relay_t *)arg;
    (void)tpcb; (void)err;

    if (!p) {
        r->server_closed = true;
        if (r->client && !r->client_closed) {
            tcp_shutdown(r->client, 0, 1);
        }
        if (r->client_closed) relay_cleanup(r);
        return ERR_OK;
    }

    if (!r->client || r->client_closed) {
        pbuf_free(p);
        return ERR_OK;
    }

    err_t werr = tcp_write(r->client, p->payload, p->tot_len, TCP_WRITE_FLAG_COPY);
    if (werr == ERR_OK) {
        tcp_recved(r->server, p->tot_len);
        tcp_output(r->client);
    } else if (werr == ERR_MEM) {
        pbuf_free(p);
        return ERR_MEM;
    }

    pbuf_free(p);
    return ERR_OK;
}

static void relay_client_err(void *arg, err_t err) {
    tcp_relay_t *r = (tcp_relay_t *)arg;
    (void)err;
    r->client = NULL;  /* lwIP already freed it */
    r->client_closed = true;
    if (r->server) {
        tcp_arg(r->server, NULL);
        tcp_recv(r->server, NULL);
        tcp_err(r->server, NULL);
        tcp_abort(r->server);
        r->server = NULL;
    }
    tcp_relay_t **pp = &active_relays;
    while (*pp && *pp != r) pp = &(*pp)->next;
    if (*pp) *pp = r->next;
    free(r);
}

static void relay_server_err(void *arg, err_t err) {
    tcp_relay_t *r = (tcp_relay_t *)arg;
    (void)err;
    r->server = NULL;
    r->server_closed = true;
    if (r->client) {
        tcp_arg(r->client, NULL);
        tcp_recv(r->client, NULL);
        tcp_err(r->client, NULL);
        tcp_abort(r->client);
        r->client = NULL;
    }
    tcp_relay_t **pp = &active_relays;
    while (*pp && *pp != r) pp = &(*pp)->next;
    if (*pp) *pp = r->next;
    free(r);
}

static err_t relay_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg; (void)tpcb; (void)len;
    return ERR_OK;
}

static err_t relay_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg; (void)tpcb; (void)len;
    return ERR_OK;
}

/* Server-side connect completed  -> wire up relay */
static err_t relay_server_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    tcp_relay_t *r = (tcp_relay_t *)arg;

    if (err != ERR_OK || !tpcb) {
        /* Connection to ESP32 failed  -> close client */
        if (r->client) {
            tcp_arg(r->client, NULL);
            tcp_recv(r->client, NULL);
            tcp_err(r->client, NULL);
            tcp_close(r->client);
            r->client = NULL;
        }
        tcp_relay_t **pp = &active_relays;
        while (*pp && *pp != r) pp = &(*pp)->next;
        if (*pp) *pp = r->next;
        free(r);
        return ERR_OK;
    }

    /* Server connected  -- wire up bidirectional relay */
    tcp_recv(r->server, relay_server_recv);
    tcp_err(r->server, relay_server_err);
    tcp_sent(r->server, relay_server_sent);

    /* Now accept data from client */
    tcp_recv(r->client, relay_client_recv);
    tcp_err(r->client, relay_client_err);
    tcp_sent(r->client, relay_client_sent);

    return ERR_OK;
}

/* New TCP connection from host  -> start relay to ESP32 */
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_listener_t *l = (tcp_listener_t *)arg;

    if (err != ERR_OK || !newpcb) return ERR_VAL;

    if (!ppp_up) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    int count = 0;
    for (tcp_relay_t *r = active_relays; r; r = r->next) {
        if (r->port == l->port) count++;
    }
    if (count >= NAT_MAX_TCP_RELAYS) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    tcp_relay_t *relay = calloc(1, sizeof(tcp_relay_t));
    if (!relay) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    relay->client = newpcb;
    relay->port = l->port;
    relay->next = active_relays;
    active_relays = relay;

    tcp_arg(newpcb, relay);
    /* Don't set recv callback yet  -- wait for server connection */
    tcp_err(newpcb, relay_client_err);

    struct tcp_pcb *server_pcb = tcp_new();
    if (!server_pcb) {
        relay_cleanup(relay);
        return ERR_ABRT;
    }

    relay->server = server_pcb;
    tcp_arg(server_pcb, relay);

    ip_addr_t dst;
    ip_addr_copy_from_ip4(dst, peer_ip);
    err_t cerr = tcp_connect(server_pcb, &dst, l->port, relay_server_connected);
    if (cerr != ERR_OK) {
        relay->server = NULL;
        tcp_abort(server_pcb);
        relay_cleanup(relay);
        return ERR_ABRT;
    }

    return ERR_OK;
}

/* UDP relay */

typedef struct udp_relay {
    struct udp_pcb *listen_pcb;     /* Bound on NCM side */
    struct udp_pcb *forward_pcb;    /* For sending to/receiving from ESP32 */
    uint16_t port;
    ip_addr_t last_client_addr;     /* Source addr of last host packet */
    uint16_t last_client_port;      /* Source port of last host packet */
    struct udp_relay *next;
} udp_relay_t;

static udp_relay_t *udp_relays = NULL;

/*
 * Try to compress a DDP packet's pixel data in-place within the pbuf.
 * Returns a (possibly new) pbuf with compressed payload, or the original
 * if compression wasn't beneficial. Caller must free the returned pbuf.
 */
static struct pbuf *ddp_try_compress(struct pbuf *p) {
    if (p->tot_len < DDP_HEADER_LEN) return p;

    uint8_t hdr[DDP_HEADER_LEN];
    pbuf_copy_partial(p, hdr, DDP_HEADER_LEN, 0);

    uint8_t flags = hdr[0];
    if (hdr[2] & DDP_TYPE_COMPRESSED) return p;

    uint16_t data_len = (hdr[8] << 8) | hdr[9];
    if (data_len == 0 || data_len > DDP_CHANNELS_PER_PACKET) return p;
    if (p->tot_len < DDP_HEADER_LEN + data_len) return p;

    uint8_t pixel_data[DDP_CHANNELS_PER_PACKET];
    pbuf_copy_partial(p, pixel_data, data_len, DDP_HEADER_LEN);

    bool is_push = flags & DDP_FLAGS_PUSH;

    /* Send uncompressed keyframe every 30 frames or on first frame */
    if (!ddp_has_prev || ddp_frame_count % 30 == 0) {
        memcpy(ddp_prev_frame, pixel_data, data_len);
        ddp_has_prev = true;
        if (is_push) ddp_frame_count++;
        return p;
    }

    size_t comp_len;
    uint8_t comp_type;
    if (!rle_encode_adaptive(pixel_data, ddp_prev_frame, data_len,
                             ddp_comp_buf, ddp_workspace, &comp_len, &comp_type)) {
        memcpy(ddp_prev_frame, pixel_data, data_len);
        if (is_push) ddp_frame_count++;
        return p;
    }

    memcpy(ddp_prev_frame, pixel_data, data_len);
    if (is_push) ddp_frame_count++;

    hdr[2] |= DDP_TYPE_COMPRESSED;
    hdr[1] = (hdr[1] & 0x0F) | comp_type;
    hdr[8] = (comp_len >> 8) & 0xFF;
    hdr[9] = comp_len & 0xFF;

    uint16_t new_total = DDP_HEADER_LEN + (uint16_t)comp_len;
    struct pbuf *cp = pbuf_alloc(PBUF_TRANSPORT, new_total, PBUF_RAM);
    if (!cp) return p;

    memcpy(cp->payload, hdr, DDP_HEADER_LEN);
    memcpy((uint8_t *)cp->payload + DDP_HEADER_LEN, ddp_comp_buf, comp_len);

    pbuf_free(p);
    return cp;
}

static void udp_ncm_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    udp_relay_t *r = (udp_relay_t *)arg;
    (void)pcb;

    if (!ppp_up || !p) {
        if (p) pbuf_free(p);
        return;
    }

    ip_addr_copy(r->last_client_addr, *addr);
    r->last_client_port = port;

    /* DDP: compress pixel data before forwarding over PPP */
    if (r->port == DDP_DEFAULT_PORT) {
        p = ddp_try_compress(p);
    }

    ip_addr_t dst;
    ip_addr_copy_from_ip4(dst, peer_ip);
    udp_sendto(r->forward_pcb, p, &dst, r->port);

    pbuf_free(p);
}

/* Received from ESP32 on PPP  -> forward back to host via NCM */
static void udp_ppp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    udp_relay_t *r = (udp_relay_t *)arg;
    (void)pcb; (void)addr; (void)port;

    if (!p) return;

    if (r->last_client_port != 0) {
        udp_sendto_if(r->listen_pcb, p, &r->last_client_addr,
                      r->last_client_port, ncm_nif);
    }

    pbuf_free(p);
}

void nat_relay_init(struct netif *netif) {
    ncm_nif = netif;
    ip4_addr_set_zero(&peer_ip);
}

void nat_relay_set_peer(const ip4_addr_t *esp32_ip) {
    ip4_addr_copy(peer_ip, *esp32_ip);
}

void nat_relay_set_ppp_up(bool up) {
    ppp_up = up;
    if (!up) {
        /* Close all active TCP relays on link down */
        while (active_relays) {
            relay_cleanup(active_relays);
        }
    }
}

void nat_relay_add_tcp(uint16_t port) {
    struct tcp_pcb *lpcb = tcp_new();
    if (!lpcb) return;

    /* Bind to NCM netif's IP only (not all interfaces) */
    ip_addr_t bind_addr;
    ip_addr_copy_from_ip4(bind_addr, *netif_ip4_addr(ncm_nif));
    tcp_bind(lpcb, &bind_addr, port);

    lpcb = tcp_listen(lpcb);
    if (!lpcb) return;

    tcp_listener_t *l = calloc(1, sizeof(tcp_listener_t));
    if (!l) {
        tcp_close(lpcb);
        return;
    }
    l->listen_pcb = lpcb;
    l->port = port;
    l->next = tcp_listeners;
    tcp_listeners = l;

    tcp_arg(lpcb, l);
    tcp_accept(lpcb, tcp_accept_cb);
}

void nat_relay_add_udp(uint16_t port) {
    udp_relay_t *r = calloc(1, sizeof(udp_relay_t));
    if (!r) return;

    r->port = port;

    /* Listen PCB  -- bound on NCM side */
    r->listen_pcb = udp_new();
    if (!r->listen_pcb) { free(r); return; }

    ip_addr_t bind_addr;
    ip_addr_copy_from_ip4(bind_addr, *netif_ip4_addr(ncm_nif));
    udp_bind(r->listen_pcb, &bind_addr, port);
    udp_recv(r->listen_pcb, udp_ncm_recv, r);

    /* Forward PCB  -- for ESP32 communication */
    r->forward_pcb = udp_new();
    if (!r->forward_pcb) {
        udp_remove(r->listen_pcb);
        free(r);
        return;
    }
    /* Bind to ephemeral port (any)  -- responses come back here */
    udp_bind(r->forward_pcb, IP4_ADDR_ANY, 0);
    udp_recv(r->forward_pcb, udp_ppp_recv, r);

    r->next = udp_relays;
    udp_relays = r;
}
