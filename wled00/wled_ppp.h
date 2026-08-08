#pragma once
#ifdef WLED_USE_PPP

#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"
#include "driver/uart.h"

// PPP configuration defaults (overridable via build flags)
#ifndef PPP_BAUD
#define PPP_BAUD 1500000
#endif
#ifndef PPP_UART_NUM
#define PPP_UART_NUM UART_NUM_0
#endif
#ifndef PPP_TX_PIN
#define PPP_TX_PIN UART_PIN_NO_CHANGE
#endif
#ifndef PPP_RX_PIN
#define PPP_RX_PIN UART_PIN_NO_CHANGE
#endif

// Link-local IPs for PPP tunnel
#ifndef PPP_OUR_IP
#define PPP_OUR_IP "169.254.7.1"
#endif
#ifndef PPP_THEIR_IP
#define PPP_THEIR_IP "169.254.7.2"
#endif

#ifndef PPP_RX_BUF_SIZE
#define PPP_RX_BUF_SIZE 2048
#endif

extern esp_netif_t *ppp_netif;
extern volatile bool ppp_connected;

void initPPP();

#endif // WLED_USE_PPP
