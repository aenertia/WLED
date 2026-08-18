#pragma once
// Transport flags imply PPP
#ifdef WLED_USE_PPP_UART
  #ifndef WLED_USE_PPP
    #define WLED_USE_PPP
  #endif
#endif
#ifdef WLED_USE_PPP

#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"

#ifdef WLED_USE_PPP_UART
#include "driver/uart.h"

// PPP UART configuration defaults (overridable via build flags)
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
#endif // WLED_USE_PPP_UART

// --- Per-transport IP addresses ---

// UART transport IPs
#ifdef WLED_USE_PPP_UART
  #ifndef PPP_UART_OUR_IP
    #define PPP_UART_OUR_IP    "169.254.7.1"
  #endif
  #ifndef PPP_UART_THEIR_IP
    #define PPP_UART_THEIR_IP  "169.254.7.2"
  #endif
#endif

// Backward compat aliases (primary transport)
#ifdef WLED_USE_PPP_UART
  #define PPP_OUR_IP    PPP_UART_OUR_IP
  #define PPP_THEIR_IP  PPP_UART_THEIR_IP
#endif

// PPP DNS hostname — separate from WiFi mDNS (wled.local).
// PPP responder answers this name; WiFi mDNS handled by ESP-IDF stack.
#ifndef PPP_DNS_HOSTNAME
#define PPP_DNS_HOSTNAME "wled-ppp.local"
#endif

// PPP MRU negotiation — desired MRU advertised in LCP Configure-Request.
// PPP_MRU is the compile-time lwIP define (set in lwipopts.h or build flags).
// WLED_PPP_WANTED_MRU is the runtime value we set on the ppp_pcb before listen/connect.
// Must differ from PPP_DEFMRU (1500) for lcp.c to include MRU in ConfReq.
#ifndef WLED_PPP_WANTED_MRU
  #ifdef PPP_MRU
    #define WLED_PPP_WANTED_MRU PPP_MRU
  #else
    #define WLED_PPP_WANTED_MRU 1500
  #endif
#endif

// W1.1: Auto-size UART RX buffer from MRU. PPP HDLC byte-stuffing can expand
// a frame up to 2x in worst case (0x7D/0x7E escaping). Buffer must hold 2+
// complete PPP frames for reliable operation at sustained throughput.
#ifndef PPP_RX_BUF_SIZE
  #if WLED_PPP_WANTED_MRU > 1500
    #define PPP_RX_BUF_SIZE (WLED_PPP_WANTED_MRU * 3)
  #else
    #define PPP_RX_BUF_SIZE 8192
  #endif
#endif

#ifndef PPP_TX_BUF_SIZE
#define PPP_TX_BUF_SIZE 2048
#endif

// W2.1: Bandwidth budget in bytes/sec. 8N1 = 10 bits/byte, ~15% PPP overhead.
// Used for diagnostics — warns when sustained throughput exceeds 80% of budget.
#define PPP_BW_BUDGET_BYTES ((PPP_BAUD / 10) * 85 / 100)

// Transport connection bitmask for WiFi STA control
#define PPP_TRANSPORT_UART  0x01
extern uint8_t ppp_transport_mask;

void pppTransportConnected(uint8_t transport);
void pppTransportDisconnected(uint8_t transport);

// Per-transport netif globals (defined in wled_ppp.cpp)
#ifdef WLED_USE_PPP_UART
  extern esp_netif_t *ppp_netif_uart;
#endif
// Backward compat alias — points to primary netif
extern esp_netif_t *ppp_netif;
extern volatile bool ppp_connected;

void initPPP();

// Multi-netif PPP lifecycle
esp_netif_t* initPPPNetif(
    esp_err_t (*transmit_fn)(void *, void *, size_t),
    const char *our_ip,     // e.g. "169.254.7.1"
    const char *their_ip,   // e.g. "169.254.7.2"
    const char *if_key      // e.g. "PPP_UART" or "PPP_BLE" — must be unique
);
void teardownPPPNetif(esp_netif_t **netif_ptr);

void startDnsResponder();
void stopDnsResponder();

#endif // WLED_USE_PPP
