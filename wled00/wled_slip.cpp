#ifdef WLED_USE_SLIP
#include "wled.h"
#include "wled_slip.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"

static const char *TAG = "WLED_SLIP";

volatile bool slip_connected = false;
static struct netif slip_netif;
static uint8_t slip_rx_pkt[1600];
static int slip_rx_pos = 0;
static bool slip_rx_esc = false;

static err_t slip_netif_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    (void)ipaddr;
    uint8_t end = SLIP_END;
    uart_write_bytes(SLIP_UART_NUM, &end, 1);
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        uint8_t *data = (uint8_t *)q->payload;
        for (int i = 0; i < q->len; i++) {
            switch (data[i]) {
                case SLIP_END:
                    uart_write_bytes(SLIP_UART_NUM, (const uint8_t[]){SLIP_ESC, SLIP_ESC_END}, 2);
                    break;
                case SLIP_ESC:
                    uart_write_bytes(SLIP_UART_NUM, (const uint8_t[]){SLIP_ESC, SLIP_ESC_ESC}, 2);
                    break;
                default:
                    uart_write_bytes(SLIP_UART_NUM, &data[i], 1);
                    break;
            }
        }
    }
    uart_write_bytes(SLIP_UART_NUM, &end, 1);
    return ERR_OK;
}

static err_t slip_netif_init(struct netif *nif) {
    nif->name[0] = 's';
    nif->name[1] = 'l';
    nif->output = slip_netif_output;
    nif->mtu = 1500;
    nif->flags = NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

static void slip_rx_packet(uint8_t *data, int len) {
    if (len < 20) return;
    struct pbuf *p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, data, len);
    if (slip_netif.input(p, &slip_netif) != ERR_OK) {
        pbuf_free(p);
    }
}

static void slip_process_byte(uint8_t b) {
    if (slip_rx_esc) {
        slip_rx_esc = false;
        switch (b) {
            case SLIP_ESC_END: b = SLIP_END; break;
            case SLIP_ESC_ESC: b = SLIP_ESC; break;
            default: break;
        }
        if (slip_rx_pos < (int)sizeof(slip_rx_pkt))
            slip_rx_pkt[slip_rx_pos++] = b;
        return;
    }
    switch (b) {
        case SLIP_END:
            if (slip_rx_pos > 0) {
                slip_rx_packet(slip_rx_pkt, slip_rx_pos);
                slip_rx_pos = 0;
            }
            break;
        case SLIP_ESC:
            slip_rx_esc = true;
            break;
        default:
            if (slip_rx_pos < (int)sizeof(slip_rx_pkt))
                slip_rx_pkt[slip_rx_pos++] = b;
            break;
    }
}

static void slip_rx_task(void *arg) {
    uint8_t buf[256];
    for (;;) {
        int len = uart_read_bytes(SLIP_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                slip_process_byte(buf[i]);
            }
        }
    }
}

void initSLIP() {
    ESP_LOGI(TAG, "Initializing SLIP over UART%d at %d baud", SLIP_UART_NUM, SLIP_BAUD);

    uart_config_t uart_config = {};
    uart_config.baud_rate = SLIP_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(SLIP_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SLIP_UART_NUM, SLIP_TX_PIN, SLIP_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SLIP_UART_NUM, SLIP_RX_BUF_SIZE * 2,
                                         SLIP_RX_BUF_SIZE * 2, 0, NULL, 0));

    ESP_LOGI(TAG, "Waiting for serial port to stabilize...");
    delay(3000);
    uart_flush_input(SLIP_UART_NUM);

    ip4_addr_t ipaddr, netmask, gw;
    ip4addr_aton(SLIP_OUR_IP, &ipaddr);
    ip4addr_aton(SLIP_NETMASK, &netmask);
    ip4addr_aton(SLIP_THEIR_IP, &gw);

    netif_add(&slip_netif, &ipaddr, &netmask, &gw, NULL, slip_netif_init, ip_input);
    netif_set_default(&slip_netif);
    netif_set_up(&slip_netif);
    netif_set_link_up(&slip_netif);

    xTaskCreatePinnedToCore(slip_rx_task, "slip_rx", 4096, NULL, 5, NULL, 0);

    slip_connected = true;
    ESP_LOGI(TAG, "SLIP initialized: %s (gw %s)", SLIP_OUR_IP, SLIP_THEIR_IP);
}

#endif
