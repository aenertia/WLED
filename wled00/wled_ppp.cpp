#ifdef WLED_USE_PPP
#include "wled.h"
#include "wled_ppp.h"
#include "netif/ppp/ppp.h"

static const char *TAG = "WLED_PPP";

esp_netif_t *ppp_netif = nullptr;
volatile bool ppp_connected = false;

static volatile uint32_t ppp_rx_bytes = 0;
static volatile uint32_t ppp_tx_bytes = 0;
static volatile uint32_t ppp_tx_calls = 0;
static volatile uint32_t ppp_rx_fed = 0;
static volatile uint32_t ppp_rx_errs = 0;
static volatile uint32_t ppp_restarts = 0;

// Transmit callback — lwIP PPP calls this to send HDLC frames out UART
static esp_err_t ppp_transmit_cb(void *h, void *buffer, size_t len)
{
    ppp_tx_calls++;
    ppp_tx_bytes += len;
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

// Restart PPP on PHASE_DEAD. ESP-IDF's on_ppp_status_changed() returns early
// for PPPERR_CONNECT, skipping NETIF_PPP_STATUS posting. Only phase events
// (via on_ppp_notify_phase) reliably fire — use NETIF_PPP_PHASE_DEAD.
static void ppp_status_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (event_id == NETIF_PPP_PHASE_DEAD) {
        ppp_connected = false;
        ppp_restarts++;
        char diag[128];
        snprintf(diag, sizeof(diag),
                 "\r\n[PPP#%lu rx=%lu fed=%lu tx=%lu txc=%lu]\r\n",
                 ppp_restarts, ppp_rx_bytes, ppp_rx_fed,
                 ppp_tx_bytes, ppp_tx_calls);
        uart_write_bytes(PPP_UART_NUM, diag, strlen(diag));
        uart_wait_tx_done(PPP_UART_NUM, pdMS_TO_TICKS(200));
        esp_netif_action_start(ppp_netif, 0, 0, NULL);
    }
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
            ppp_connected = false;
            ppp_restarts++;
            #ifdef WLED_ENABLE_ARGB_PASSTHROUGH
            startARGBPassthrough();
            #endif
            char diag[128];
            snprintf(diag, sizeof(diag),
                     "\r\n[PPP#%lu rx=%lu fed=%lu tx=%lu txc=%lu]\r\n",
                     ppp_restarts, ppp_rx_bytes, ppp_rx_fed,
                     ppp_tx_bytes, ppp_tx_calls);
            uart_write_bytes(PPP_UART_NUM, diag, strlen(diag));
            uart_wait_tx_done(PPP_UART_NUM, pdMS_TO_TICKS(200));
            esp_netif_action_start(ppp_netif, 0, 0, NULL);
        }
    }
}

// UART RX task — feeds received bytes into PPP stack
static void ppp_rx_task(void *arg)
{
    uint8_t buf[PPP_RX_BUF_SIZE];
    for (;;) {
        int len = uart_read_bytes(PPP_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(1));
        if (len > 0 && ppp_netif) {
            ppp_rx_bytes += len;
            ppp_rx_fed++;
            esp_netif_receive(ppp_netif, buf, len, NULL);
        }
    }
}

void initPPP()
{
    ESP_ERROR_CHECK(esp_netif_init());
    { esp_err_t e = esp_event_loop_create_default();
      if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e); }

    ESP_LOGI(TAG, "Initializing PPP over UART%d at %d baud", PPP_UART_NUM, PPP_BAUD);

    if (PPP_UART_NUM == UART_NUM_0) {
        Serial.end();
    }

    // Configure UART
    uart_config_t uart_config = {};
    uart_config.baud_rate = PPP_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(PPP_UART_NUM, &uart_config));
    // Explicit pin assignment — don't rely on IO_MUX defaults persisting
    // M5StickC ESP32-PICO-D4: UART0 TX=GPIO1, RX=GPIO3 (via FTDI FT232)
    ESP_ERROR_CHECK(uart_set_pin(PPP_UART_NUM, 1, 3,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(PPP_UART_NUM, PPP_RX_BUF_SIZE * 2,
                                         PPP_RX_BUF_SIZE * 2, 0, NULL, 0));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                &ppp_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                                &ppp_status_handler, NULL));

    // Create PPP netif with driver post-attach
    static esp_netif_driver_base_t driver_base = {};
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

    delay(500);

    // Start RX task BEFORE PPP — ppp_listen() is passive, needs RX ready
    xTaskCreatePinnedToCore(ppp_rx_task, "ppp_rx", 8192, NULL, 5, NULL, 0);

    esp_netif_action_start(ppp_netif, 0, 0, NULL);

    // Periodic diagnostic task — outputs counters every 30s while PPP is down.
    // Runs independently of events (bypasses the ESP-IDF event delivery issue).
    xTaskCreatePinnedToCore([](void *) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            if (!ppp_connected && ppp_netif) {
                char d[128];
                snprintf(d, sizeof(d),
                         "\r\n[DIAG rx=%lu fed=%lu tx=%lu txc=%lu rst=%lu up=%lus]\r\n",
                         ppp_rx_bytes, ppp_rx_fed, ppp_tx_bytes, ppp_tx_calls,
                         ppp_restarts, millis() / 1000);
                uart_write_bytes(PPP_UART_NUM, d, strlen(d));
            }
        }
    }, "ppp_diag", 2048, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "PPP listening on UART%d", PPP_UART_NUM);
}

#endif // WLED_USE_PPP
