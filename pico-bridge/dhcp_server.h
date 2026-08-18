#ifndef DHCP_SERVER_H_
#define DHCP_SERVER_H_

#include "lwip/netif.h"

/*
 * Minimal single-client DHCP server for the NCM interface.
 * Assigns one IP (client_ip) with the netif's IP as gateway.
 * Lease is effectively infinite (no expiry tracking).
 */
void dhcp_server_init(struct netif *netif, const ip4_addr_t *client_ip);
void dhcp_server_deinit(void);

#endif
