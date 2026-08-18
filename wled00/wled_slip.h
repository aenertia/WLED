#pragma once
#ifdef WLED_USE_SLIP

#include "esp_netif.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "lwip/netif.h"

#ifndef SLIP_BAUD
#define SLIP_BAUD 1500000
#endif
#ifndef SLIP_UART_NUM
#define SLIP_UART_NUM UART_NUM_0
#endif
#ifndef SLIP_TX_PIN
#define SLIP_TX_PIN UART_PIN_NO_CHANGE
#endif
#ifndef SLIP_RX_PIN
#define SLIP_RX_PIN UART_PIN_NO_CHANGE
#endif

#ifndef SLIP_OUR_IP
#define SLIP_OUR_IP "169.254.7.1"
#endif
#ifndef SLIP_THEIR_IP
#define SLIP_THEIR_IP "169.254.7.2"
#endif
#ifndef SLIP_NETMASK
#define SLIP_NETMASK "255.255.255.0"
#endif

#ifndef SLIP_RX_BUF_SIZE
#define SLIP_RX_BUF_SIZE 2048
#endif

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

extern volatile bool slip_connected;

void initSLIP();

#endif
