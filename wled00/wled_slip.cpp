#ifdef WLED_USE_SLIP
#include "wled.h"
#include "wled_slip.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/tcpip.h"

static const char *TAG = "WLED_SLIP";

volatile bool slip_connected = false;
static struct netif slip_netif;
static uint8_t slip_rx_pkt[1600];
static int slip_rx_pos = 0;
static bool slip_rx_esc = false;

static uint8_t slip_tx_buf[3200];

static err_t slip_netif_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    (void)nif; (void)ipaddr;
    int pos = 0;
    slip_tx_buf[pos++] = SLIP_END;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        uint8_t *data = (uint8_t *)q->payload;
        for (int i = 0; i < q->len; i++) {
            switch (data[i]) {
                case SLIP_END: slip_tx_buf[pos++] = SLIP_ESC; slip_tx_buf[pos++] = SLIP_ESC_END; break;
                case SLIP_ESC: slip_tx_buf[pos++] = SLIP_ESC; slip_tx_buf[pos++] = SLIP_ESC_ESC; break;
                default:       slip_tx_buf[pos++] = data[i]; break;
            }
        }
    }
    slip_tx_buf[pos++] = SLIP_END;
    Serial.write(slip_tx_buf, pos);
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
    if (len < 20 || !slip_netif.input) return;
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
    for (;;) {
        int avail = Serial.available();
        if (avail > 0) {
            int count = (avail > 256) ? 256 : avail;
            while (count-- > 0) {
                slip_process_byte(Serial.read());
            }
            taskYIELD();
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void initSLIP() {
    ESP_LOGI(TAG, "Initializing SLIP over UART%d at %d baud", SLIP_UART_NUM, SLIP_BAUD);

    Serial.end();
    Serial.begin(SLIP_BAUD);
    delay(3000);
    while (Serial.available()) Serial.read();

    ip4_addr_t ip, mask, gw;
    ip4addr_aton(SLIP_OUR_IP, &ip);
    ip4addr_aton(SLIP_NETMASK, &mask);
    ip4addr_aton(SLIP_THEIR_IP, &gw);

    netif_add(&slip_netif, &ip, &mask, &gw, NULL, slip_netif_init, tcpip_input);
    netif_set_default(&slip_netif);
    netif_set_up(&slip_netif);

    xTaskCreatePinnedToCore(slip_rx_task, "slip_rx", 4096, NULL, 5, NULL, 0);

    slip_connected = true;
    ESP_LOGI(TAG, "SLIP ready — %s gw %s", SLIP_OUR_IP, SLIP_THEIR_IP);
}

#endif
