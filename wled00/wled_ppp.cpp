#ifdef WLED_USE_PPP
#include "wled.h"
#include "wled_ppp.h"

static const char *TAG = "WLED_PPP";

esp_netif_t *ppp_netif = nullptr;
volatile bool ppp_connected = false;

// Transmit callback — lwIP PPP calls this to send HDLC frames out UART
static esp_err_t ppp_transmit_cb(void *h, void *buffer, size_t len)
{
    int written = uart_write_bytes(PPP_UART_NUM, buffer, len);
    return (written == (int)len) ? ESP_OK : ESP_FAIL;
}

// Post-attach callback — registers transmit function with esp_netif
static esp_err_t ppp_driver_post_attach(esp_netif_t *netif, esp_netif_iodriver_handle h)
{
    esp_netif_driver_ifconfig_t driver_cfg = {};
    driver_cfg.handle = (void *)(intptr_t)PPP_UART_NUM;
    driver_cfg.transmit = ppp_transmit_cb;
    return esp_netif_set_driver_config(netif, &driver_cfg);
}

// IP event handler
static void ppp_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (base == IP_EVENT) {
        if (event_id == IP_EVENT_PPP_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "PPP Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            ppp_connected = true;
            #ifdef WLED_ENABLE_ARGB_PASSTHROUGH
            stopARGBPassthrough();
            #endif
        } else if (event_id == IP_EVENT_PPP_LOST_IP) {
            ESP_LOGI(TAG, "PPP Lost IP");
            ppp_connected = false;
            #ifdef WLED_ENABLE_ARGB_PASSTHROUGH
            startARGBPassthrough();
            #endif
        }
    }
}

// UART RX task — feeds received bytes into PPP stack
static void ppp_rx_task(void *arg)
{
    uint8_t buf[PPP_RX_BUF_SIZE];
    for (;;) {
        int len = uart_read_bytes(PPP_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0 && ppp_netif) {
            esp_netif_receive(ppp_netif, buf, len, NULL);
        }
    }
}

void initPPP()
{
    ESP_LOGI(TAG, "Initializing PPP over UART%d at %d baud", PPP_UART_NUM, PPP_BAUD);

    // Configure UART
    uart_config_t uart_config = {};
    uart_config.baud_rate = PPP_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(PPP_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(PPP_UART_NUM, PPP_TX_PIN, PPP_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(PPP_UART_NUM, PPP_RX_BUF_SIZE * 2,
                                         PPP_RX_BUF_SIZE * 2, 0, NULL, 0));

    // Register IP event handler
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                &ppp_event_handler, NULL));

    // Create PPP netif with driver post-attach
    esp_netif_driver_base_t driver_base = {};
    driver_base.post_attach = ppp_driver_post_attach;

    esp_netif_inherent_config_t base_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_netif_cfg.route_prio = 128;  // Higher than WiFi (100) to prefer PPP

    esp_netif_config_t netif_ppp_config = {};
    netif_ppp_config.base = &base_netif_cfg;
    netif_ppp_config.driver = NULL;
    netif_ppp_config.stack = ESP_NETIF_NETSTACK_DEFAULT_PPP;

    ppp_netif = esp_netif_new(&netif_ppp_config);
    assert(ppp_netif);

    // Attach driver (triggers post_attach callback)
    ESP_ERROR_CHECK(esp_netif_attach(ppp_netif, &driver_base));

    // Configure PPP server mode with static IPs
    esp_netif_ppp_config_t ppp_config = {};
    ppp_config.ppp_phase_event_enabled = true;
    ppp_config.ppp_error_event_enabled = true;
#ifdef CONFIG_LWIP_PPP_SERVER_SUPPORT
    esp_netif_str_to_ip4(PPP_OUR_IP, &ppp_config.ppp_our_ip4_addr);
    esp_netif_str_to_ip4(PPP_THEIR_IP, &ppp_config.ppp_their_ip4_addr);
#endif
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(ppp_netif, &ppp_config));

    // Wait for FTDI serial port to stabilize after auto-reset
    // When the host opens /dev/ttyUSB0, the FTDI's DTR/RTS lines reset the ESP32.
    // On the second boot, the port is already open — no more resets.
    // This delay lets the FTDI settle and any baud-rate-mismatch garbage accumulate.
    ESP_LOGI(TAG, "Waiting for serial port to stabilize...");
    delay(3000);
    uart_flush_input(PPP_UART_NUM);

    // Start PPP connection (server waits for client LCP)
    esp_netif_action_start(ppp_netif, 0, 0, NULL);

    // Start RX task on core 0 (LED output uses core 1 via RMT)
    xTaskCreatePinnedToCore(ppp_rx_task, "ppp_rx", 8192, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "PPP initialized, waiting for host pppd...");
}

#endif // WLED_USE_PPP
