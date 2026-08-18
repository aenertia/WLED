#ifdef WLED_USE_PPP
#include "wled.h"
#include "wled_ppp.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include <ctype.h>

static const char *TAG = "WLED_PPP";

// Per-transport netif globals
#ifdef WLED_USE_PPP_UART
esp_netif_t *ppp_netif_uart = nullptr;
#endif
// Backward compat alias — points to primary netif
esp_netif_t *ppp_netif = nullptr;
volatile bool ppp_connected = false;

static uint32_t ppp_restarts = 0;

// Transport connection bitmask for WiFi STA control
uint8_t ppp_transport_mask = 0;

void pppTransportConnected(uint8_t transport)
{
    ppp_transport_mask |= transport;
}

void pppTransportDisconnected(uint8_t transport)
{
    ppp_transport_mask &= ~transport;
}

// Per-netif driver: extends esp_netif_driver_base_t with transmit function.
// Heap-allocated in initPPPNetif(), freed in teardownPPPNetif().
struct ppp_driver_t {
    esp_netif_driver_base_t base;
    esp_err_t (*transmit_fn)(void *, void *, size_t);
};

#ifdef WLED_USE_PPP_UART
// ---- UART-specific counters and transmit ----
static uint32_t ppp_rx_bytes = 0;
static uint32_t ppp_tx_bytes = 0;
static uint32_t ppp_tx_calls = 0;
static uint32_t ppp_rx_fed = 0;
static uint32_t ppp_rx_errs = 0;
static uint32_t ppp_uart_overflows = 0;
static uint32_t ppp_rx_bytes_sec = 0;
static uint32_t ppp_tx_bytes_sec = 0;
static QueueHandle_t ppp_uart_event_queue = NULL;

// UART transmit callback — lwIP PPP calls this to send HDLC frames out UART
static esp_err_t ppp_transmit_cb(void *h, void *buffer, size_t len)
{
    __atomic_fetch_add(&ppp_tx_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ppp_tx_bytes, len, __ATOMIC_RELAXED);
    int written = uart_write_bytes(PPP_UART_NUM, buffer, len);
    return (written == (int)len) ? ESP_OK : ESP_FAIL;
}
#endif // WLED_USE_PPP_UART

// Post-attach callback — registers per-netif transmit function with esp_netif
static esp_err_t ppp_driver_post_attach(esp_netif_t *netif, esp_netif_iodriver_handle h)
{
    ppp_driver_t *drv = (ppp_driver_t *)h;
    esp_netif_driver_ifconfig_t driver_cfg = {};
    driver_cfg.handle = h;
    driver_cfg.transmit = drv->transmit_fn;
    return esp_netif_set_driver_config(netif, &driver_cfg);
}


// ---- Minimal DNS responder for wled.local over PPP ----
static int dns_sock = -1;
static TaskHandle_t dns_task_handle = NULL;

// DNS response IP — parsed from PPP_OUR_IP at task start.
// Always the primary transport IP (UART preferred, else BLE).
static uint8_t dns_ip_bytes[4];

static void dns_responder_task(void *arg) {
    // Parse primary PPP IP for DNS responses
    esp_ip4_addr_t respond_addr;
    esp_netif_str_to_ip4(PPP_OUR_IP, &respond_addr);
    dns_ip_bytes[0] = (respond_addr.addr >>  0) & 0xFF;
    dns_ip_bytes[1] = (respond_addr.addr >>  8) & 0xFF;
    dns_ip_bytes[2] = (respond_addr.addr >> 16) & 0xFF;
    dns_ip_bytes[3] = (respond_addr.addr >> 24) & 0xFF;

    uint8_t buf[256];
    struct sockaddr_in client;
    socklen_t client_len;

    while (dns_sock >= 0) {
        client_len = sizeof(client);
        int n = recvfrom(dns_sock, buf, sizeof(buf), 0,
                        (struct sockaddr *)&client, &client_len);
        if (n < 12) continue;

        // Parse QNAME from question section (offset 12)
        char qname[64] = {0};
        int pos = 12, qpos = 0;
        while (pos < n && buf[pos] != 0 && qpos < 63) {
            int label_len = buf[pos++];
            if (label_len >= 0xC0) break;  // reject DNS compression pointers
            if (label_len > 63 || pos + label_len > n) break;
            if (qpos > 0) qname[qpos++] = '.';
            for (int j = 0; j < label_len && qpos < 63; j++) {
                qname[qpos++] = tolower(buf[pos++]);
            }
        }
        qname[qpos] = 0;
        pos++; // skip null terminator
        pos += 4; // skip QTYPE + QCLASS

        // Build response -- copy header + question, set flags
        uint8_t resp[256];
        if (pos > (int)sizeof(resp) - 16) continue;
        memcpy(resp, buf, pos);
        resp[2] = 0x84; // QR=1, AA=1

        if (strcasecmp(qname, PPP_DNS_HOSTNAME) == 0) {
            resp[3] = 0x00; // RCODE=0
            resp[6] = 0; resp[7] = 1; // ANCOUNT=1
            int apos = pos;
            resp[apos++] = 0xC0; resp[apos++] = 0x0C; // name pointer
            resp[apos++] = 0x00; resp[apos++] = 0x01; // TYPE A
            resp[apos++] = 0x00; resp[apos++] = 0x01; // CLASS IN
            resp[apos++] = 0x00; resp[apos++] = 0x00;
            resp[apos++] = 0x00; resp[apos++] = 0x3C; // TTL=60s
            resp[apos++] = 0x00; resp[apos++] = 0x04; // RDLENGTH=4
            resp[apos++] = dns_ip_bytes[0];
            resp[apos++] = dns_ip_bytes[1];
            resp[apos++] = dns_ip_bytes[2];
            resp[apos++] = dns_ip_bytes[3];
            sendto(dns_sock, resp, apos, 0,
                   (struct sockaddr *)&client, client_len);
        } else {
            resp[3] = 0x03; // RCODE=3 NXDOMAIN
            resp[6] = 0; resp[7] = 0; // ANCOUNT=0
            sendto(dns_sock, resp, pos, 0,
                   (struct sockaddr *)&client, client_len);
        }
    }
    vTaskDelete(NULL);
}

void startDnsResponder() {
    if (dns_sock >= 0) return;

    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) {
        ESP_LOGW(TAG, "DNS socket failed");
        return;
    }

    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    esp_ip4_addr_t bind_addr;
    esp_netif_str_to_ip4(PPP_OUR_IP, &bind_addr);
    sa.sin_addr.s_addr = bind_addr.addr;

    if (bind(dns_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        close(dns_sock);
        dns_sock = -1;
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    xTaskCreatePinnedToCore(dns_responder_task, "dns_resp", 2048, NULL, 3, &dns_task_handle, 1);
    ESP_LOGI(TAG, "DNS responder started (" PPP_DNS_HOSTNAME " -> " PPP_OUR_IP ")");
}

void stopDnsResponder() {
    if (dns_sock >= 0) {
        int s = dns_sock;
        dns_sock = -1;
        close(s);
    }
    if (dns_task_handle) {
        dns_task_handle = NULL;
    }
    ESP_LOGI(TAG, "DNS responder stopped");
}

// Restart PPP on PHASE_DEAD. ESP-IDF's on_ppp_status_changed() returns early
// for PPPERR_CONNECT, skipping NETIF_PPP_STATUS posting. Only phase events
// (via on_ppp_notify_phase) reliably fire — use NETIF_PPP_PHASE_DEAD.
static void ppp_status_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (event_id == NETIF_PPP_PHASE_DEAD) {
        __atomic_fetch_add(&ppp_restarts, 1, __ATOMIC_RELAXED);
#ifdef WLED_USE_PPP_UART
        char diag[128];
        snprintf(diag, sizeof(diag),
                 "\r\n[PPP#%lu rx=%lu fed=%lu tx=%lu txc=%lu]\r\n",
                 ppp_restarts, ppp_rx_bytes, ppp_rx_fed,
                 ppp_tx_bytes, ppp_tx_calls);
        uart_write_bytes(PPP_UART_NUM, diag, strlen(diag));
        uart_wait_tx_done(PPP_UART_NUM, pdMS_TO_TICKS(200));
        // UART is always-on: auto-restart
        if (ppp_netif_uart) {
            esp_netif_action_start(ppp_netif_uart, 0, 0, NULL);
        }
#else
        ESP_LOGW(TAG, "PPP phase dead (restart #%lu)", ppp_restarts);
#endif
    }
}

// IP event handler — tracks per-transport connect/disconnect state
static void ppp_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (base != IP_EVENT) return;

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "PPP Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ppp_connected = true;

        // Track which transport connected via bitmask
#ifdef WLED_USE_PPP_UART
        if (event->esp_netif == ppp_netif_uart)
            pppTransportConnected(PPP_TRANSPORT_UART);
#endif
        startDnsResponder();


    }
    else if (event_id == IP_EVENT_PPP_LOST_IP) {
        __atomic_fetch_add(&ppp_restarts, 1, __ATOMIC_RELAXED);

        // Determine which transport(s) lost IP by checking current state.
        // LOST_IP event data doesn't reliably carry the netif pointer
        // across ESP-IDF versions, so we re-scan.
#ifdef WLED_USE_PPP_UART
        if (ppp_netif_uart && (ppp_transport_mask & PPP_TRANSPORT_UART)) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(ppp_netif_uart, &ip) != ESP_OK || ip.ip.addr == 0) {
                pppTransportDisconnected(PPP_TRANSPORT_UART);
            }
        }
#endif
        ppp_connected = (ppp_transport_mask != 0);

        if (ppp_transport_mask == 0) {
            stopDnsResponder();

        }

        // UART auto-restart (always-on wired connection)
#ifdef WLED_USE_PPP_UART
        if (ppp_netif_uart) {
            char diag[128];
            snprintf(diag, sizeof(diag),
                     "\r\n[PPP#%lu rx=%lu fed=%lu tx=%lu txc=%lu]\r\n",
                     ppp_restarts, ppp_rx_bytes, ppp_rx_fed,
                     ppp_tx_bytes, ppp_tx_calls);
            uart_write_bytes(PPP_UART_NUM, diag, strlen(diag));
            uart_wait_tx_done(PPP_UART_NUM, pdMS_TO_TICKS(200));
            esp_netif_action_start(ppp_netif_uart, 0, 0, NULL);
        }
#endif
    }
}

// ---- Transport-agnostic PPP netif lifecycle ----

esp_netif_t* initPPPNetif(
    esp_err_t (*transmit_fn)(void *, void *, size_t),
    const char *our_ip,
    const char *their_ip,
    const char *if_key)
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                &ppp_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                                &ppp_status_handler, NULL));

    // Heap-allocate per-netif driver (freed in teardownPPPNetif)
    ppp_driver_t *drv = (ppp_driver_t *)calloc(1, sizeof(ppp_driver_t));
    assert(drv);
    drv->base.post_attach = ppp_driver_post_attach;
    drv->transmit_fn = transmit_fn;

    esp_netif_inherent_config_t base_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_netif_cfg.route_prio = 128;  // Higher than WiFi (100) to prefer PPP
    base_netif_cfg.if_key = if_key;   // Unique per netif for multi-transport

    esp_netif_config_t netif_ppp_config = {};
    netif_ppp_config.base = &base_netif_cfg;
    netif_ppp_config.driver = NULL;
    netif_ppp_config.stack = ESP_NETIF_NETSTACK_DEFAULT_PPP;

    esp_netif_t *netif = esp_netif_new(&netif_ppp_config);
    assert(netif);

    // Attach driver (triggers post_attach callback)
    ESP_ERROR_CHECK(esp_netif_attach(netif, &drv->base));

    // Configure PPP server mode with static IPs
    esp_netif_ppp_config_t ppp_config = {};
    ppp_config.ppp_phase_event_enabled = true;
    ppp_config.ppp_error_event_enabled = true;
#ifdef CONFIG_LWIP_PPP_SERVER_SUPPORT
    esp_netif_str_to_ip4(our_ip, &ppp_config.ppp_our_ip4_addr);
    esp_netif_str_to_ip4(their_ip, &ppp_config.ppp_their_ip4_addr);
#endif
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(netif, &ppp_config));

#if WLED_PPP_WANTED_MRU != 1500
    {
        struct netif *lwip_nif = (struct netif *)esp_netif_get_netif_impl(netif);
        if (lwip_nif && lwip_nif->state) {
            // netif->state is ppp_pcb* per lwIP convention (ppp.c:508).
            // Tested with ESP-IDF 5.3.4 / lwIP 2.2.0. If ESP-IDF updates
            // change ppp_pcb layout, this will silently corrupt memory.
            ppp_pcb *pcb = (ppp_pcb *)lwip_nif->state;
            pcb->lcp_wantoptions.mru = WLED_PPP_WANTED_MRU;
            pcb->lcp_allowoptions.mru = WLED_PPP_WANTED_MRU;
            ESP_LOGI(TAG, "LCP MRU set to %d", WLED_PPP_WANTED_MRU);
        }
    }
#endif

    // Advertise ourselves as DNS server during IPCP negotiation
    esp_netif_dns_info_t dns_info = {};
    esp_ip4_addr_t our_addr;
    esp_netif_str_to_ip4(our_ip, &our_addr);
    dns_info.ip.u_addr.ip4 = our_addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);

    esp_netif_action_start(netif, 0, 0, NULL);
    ESP_LOGI(TAG, "PPP netif started (if_key=%s, our=%s, their=%s)",
             if_key, our_ip, their_ip);

    return netif;
}

void teardownPPPNetif(esp_netif_t **netif_ptr)
{
    if (!netif_ptr || !*netif_ptr) return;

    // Clear transport bit for the netif being torn down
#ifdef WLED_USE_PPP_UART
    if (*netif_ptr == ppp_netif_uart)
        pppTransportDisconnected(PPP_TRANSPORT_UART);
#endif
    // Stop DNS only when all transports are down
    if (ppp_transport_mask == 0) {
        stopDnsResponder();

    }

    ESP_LOGI(TAG, "Tearing down PPP netif");
    ppp_connected = (ppp_transport_mask != 0);

    esp_netif_action_stop(*netif_ptr, 0, 0, NULL);

    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &ppp_event_handler);
    esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &ppp_status_handler);

    // Free heap-allocated per-netif driver
    void *drv = esp_netif_get_io_driver(*netif_ptr);

    esp_netif_destroy(*netif_ptr);
    free(drv);

    // Clear backward compat alias if it pointed to this netif
    if (ppp_netif == *netif_ptr) {
        ppp_netif = nullptr;
    }
    *netif_ptr = nullptr;
}

// ---- UART-specific RX task and init ----

#ifdef WLED_USE_PPP_UART
static void ppp_uart_event_task(void *arg)
{
    uart_event_t event;
    for (;;) {
        if (xQueueReceive(ppp_uart_event_queue, &event, pdMS_TO_TICKS(100))) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                __atomic_fetch_add(&ppp_uart_overflows, 1, __ATOMIC_RELAXED);
                uart_flush_input(PPP_UART_NUM);
                xQueueReset(ppp_uart_event_queue);
            }
        }
    }
}

static void ppp_rx_task(void *arg)
{
    uint8_t buf[1024];
    size_t buffered = 0;
    for (;;) {
        // Two-tier flow control to prevent UART ISR ring buffer overrun.
        // The ISR writes continuously at 1.5Mbps; if esp_netif_receive()
        // blocks (tcpip_thread mailbox full), the ring buffer fills.
        // Tier 1 (>50%): yield to let tcpip_thread drain.
        // Tier 2 (>85%): emergency flush — drop bytes rather than let
        // the ISR overrun into adjacent DRAM (FreeRTOS TCBs).
        uart_get_buffered_data_len(PPP_UART_NUM, &buffered);
        if (buffered > (PPP_RX_BUF_SIZE * 3 / 2)) {  // >75% of 2x alloc
            uart_flush_input(PPP_UART_NUM);
            __atomic_fetch_add(&ppp_uart_overflows, 1, __ATOMIC_RELAXED);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (buffered > PPP_RX_BUF_SIZE) {  // >50% of 2x alloc
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        int len = uart_read_bytes(PPP_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(1));
        if (len > 0 && ppp_netif_uart) {
            __atomic_fetch_add(&ppp_rx_bytes, len, __ATOMIC_RELAXED);
            __atomic_fetch_add(&ppp_rx_fed, 1, __ATOMIC_RELAXED);
            esp_netif_receive(ppp_netif_uart, buf, len, NULL);
        }
    }
}
#endif // WLED_USE_PPP_UART

void initPPP()
{
    // Ensure TCP/IP stack and event loop are ready — idempotent, safe to call
    // multiple times. Required because in WLED_PPP_WIFI mode, initPPP() runs
    // before any WiFi API call (which would otherwise init these lazily).
    ESP_ERROR_CHECK(esp_netif_init());
    { esp_err_t e = esp_event_loop_create_default();
      if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e); }

#ifdef WLED_USE_PPP_UART
    ESP_LOGI(TAG, "Initializing PPP over UART%d at %d baud", PPP_UART_NUM, PPP_BAUD);

    Serial.end();

    // Configure UART
    uart_config_t uart_config = {};
    uart_config.baud_rate = PPP_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(PPP_UART_NUM, &uart_config));
    // Pin assignment from build flags (PPP_TX_PIN/PPP_RX_PIN)
    // FTDI variant: defaults to UART_PIN_NO_CHANGE (IO_MUX defaults: UART0=GPIO1/3)
    // Pico variant: explicitly set to G32(TX)/G33(RX) for UART1 via Grove
    ESP_ERROR_CHECK(uart_set_pin(PPP_UART_NUM, PPP_TX_PIN, PPP_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(PPP_UART_NUM, PPP_RX_BUF_SIZE * 2,
                                         PPP_TX_BUF_SIZE, 8, &ppp_uart_event_queue, 0));

    ppp_netif_uart = initPPPNetif(ppp_transmit_cb, PPP_UART_OUR_IP,
                                   PPP_UART_THEIR_IP, "PPP_UART");
    ppp_netif = ppp_netif_uart;  // backward compat

    delay(500);

    xTaskCreatePinnedToCore(ppp_rx_task, "ppp_rx", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(ppp_uart_event_task, "ppp_evt", 2048, NULL, 4, NULL, 1);

    xTaskCreatePinnedToCore([](void *) {
        uint32_t prev_rx = 0, prev_tx = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            ppp_rx_bytes_sec = (ppp_rx_bytes - prev_rx) / 30;
            ppp_tx_bytes_sec = (ppp_tx_bytes - prev_tx) / 30;
            prev_rx = ppp_rx_bytes;
            prev_tx = ppp_tx_bytes;
            if (ppp_netif_uart) {
                char d[192];
                snprintf(d, sizeof(d),
                         "\r\n[DIAG rx=%lu tx=%lu %luKB/s ovf=%lu rst=%lu up=%lus]\r\n",
                         ppp_rx_bytes, ppp_tx_bytes,
                         (ppp_rx_bytes_sec + ppp_tx_bytes_sec) / 1024,
                         ppp_uart_overflows, ppp_restarts, millis() / 1000);
                uart_write_bytes(PPP_UART_NUM, d, strlen(d));
                if (ppp_rx_bytes_sec > (PPP_BW_BUDGET_BYTES * 80 / 100)) {
                    const char *w = "\r\n[WARN: RX near bandwidth limit]\r\n";
                    uart_write_bytes(PPP_UART_NUM, w, strlen(w));
                }
            }
        }
    }, "ppp_diag", 1536, NULL, 1, NULL, 1);

    ESP_LOGI(TAG, "PPP on UART%d @ %d baud (MRU=%d, RX buf=%d, BW budget=%d KB/s)",
             PPP_UART_NUM, PPP_BAUD, WLED_PPP_WANTED_MRU,
             PPP_RX_BUF_SIZE, PPP_BW_BUDGET_BYTES / 1024);
#else
    // No UART transport — BLE-only mode.
    // BLE PPP netif created on-demand when L2CAP connects (see wled_ppp_ble.cpp).
    ESP_LOGI(TAG, "PPP transport: BLE L2CAP CoC (deferred until connect)");
#endif // WLED_USE_PPP_UART
}

#endif // WLED_USE_PPP
