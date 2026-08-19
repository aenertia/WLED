#ifndef NAT_RELAY_H_
#define NAT_RELAY_H_

#include "lwip/ip_addr.h"
#include "lwip/netif.h"

/*
 * NAT relay  -- terminates TCP/UDP on the NCM side, forwards to ESP32 via PPP.
 *
 * TCP: Accept on listen_port  -> connect to ESP32:forward_port  -> bidirectional relay.
 * UDP: Bind on listen_port  -> forward datagrams to ESP32:forward_port  -> relay responses.
 *
 * The Pico appears as the endpoint to the host. ESP32 sees traffic from
 * Pico's PPP IP. This is classic DNAT + masquerade.
 */

/* Maximum simultaneous TCP relay connections per port */
#define NAT_MAX_TCP_RELAYS   4

/* Initialize the NAT relay subsystem. Call after both netifs are created. */
void nat_relay_init(struct netif *ncm_netif);

/* Set the ESP32 peer IP (call when PPP comes up with assigned peer address) */
void nat_relay_set_peer(const ip4_addr_t *esp32_ip);

/* Mark PPP link up/down  -- disables forwarding when down */
void nat_relay_set_ppp_up(bool up);

/* Add a TCP port relay: listen on NCM, forward to ESP32 */
void nat_relay_add_tcp(uint16_t port);

/* Add a UDP port relay: listen on NCM, forward to ESP32 */
void nat_relay_add_udp(uint16_t port);

#endif
